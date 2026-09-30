#include "segment_recorder.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/timestamp.h>
}

namespace fs = std::filesystem;

namespace {
constexpr int kReconnectSec = 10;

// Путь сегмента (ТЗ §19): recordings/camera/YYYY/MM/DD/HH/HHMM.mp4
std::string make_segment_path(const std::string& root) {
    const std::time_t now = std::time(nullptr);
    const std::tm* t = std::localtime(&now);

    char dir[64];
    std::snprintf(dir, sizeof(dir), "camera/%04d/%02d/%02d/%02d",
                  t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour);

    fs::path dir_path = fs::path(root) / dir;
    std::error_code ec;
    fs::create_directories(dir_path, ec);

    for (int suffix = 0; suffix < 100; ++suffix) {
        char name[32];
        if (suffix == 0)
            std::snprintf(name, sizeof(name), "%02d%02d.mp4", t->tm_hour, t->tm_min);
        else
            std::snprintf(name, sizeof(name), "%02d%02d_%d.mp4", t->tm_hour, t->tm_min, suffix);
        fs::path full = dir_path / name;
        if (!fs::exists(full)) return (fs::path(dir) / name).generic_string();
    }
    return (fs::path(dir) / "overflow.mp4").generic_string();
}
}  // namespace

SegmentRecorder::SegmentRecorder(db::Database& db, std::string recordings_path)
    : db_(db), recordings_path_(std::move(recordings_path)) {}

std::string SegmentRecorder::state_str() const {
    switch (state_.load()) {
        case 1: return "running";
        case 2: return "error";
        default: return "stopped";
    }
}

void SegmentRecorder::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
    spdlog::info("recorder started");
}

void SegmentRecorder::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void SegmentRecorder::reload() {
    reload_requested_ = true;
}

std::string SegmentRecorder::current_url() {
    std::lock_guard lock(mutex_);
    if (!url_provider_) return "";
    try {
        return url_provider_();
    } catch (const std::exception& e) {
        spdlog::error("recorder url provider failed: {}", e.what());
        return "";
    }
}

int SegmentRecorder::segment_duration() {
    try {
        return db_.tx([](pqxx::work& w) {
            return std::stoi(w.exec(
                "SELECT value::text FROM system_settings WHERE key='segment_duration_sec'")[0][0]
                                 .as<std::string>());
        });
    } catch (...) {
        return 300;  // дефолт 5 минут (ТЗ §73.8)
    }
}

std::string SegmentRecorder::recording_mode() {
    try {
        return db_.tx([](pqxx::work& w) {
            std::string v = w.exec(
                "SELECT value::text FROM system_settings WHERE key='recording_mode'")[0][0]
                                .as<std::string>();
            // значение хранится как JSON-строка: "\"continuous\""
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            return v;
        });
    } catch (...) {
        return "continuous";
    }
}

void SegmentRecorder::interruptible_sleep(int seconds) {
    for (int i = 0; i < seconds * 10 && running_ && !reload_requested_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void SegmentRecorder::run() {
    while (running_) {
        reload_requested_ = false;

        const std::string url = current_url();
        if (url.empty()) {
            state_ = 0;
            interruptible_sleep(5);
            continue;
        }

        if (recording_mode() != "continuous") {
            // режим "по движению" появится в Phase 4
            state_ = 0;
            interruptible_sleep(5);
            continue;
        }

        if (!record_session(url, segment_duration())) {
            state_ = 2;
            if (event_fn_) event_fn_("recording.error", "{}");
            interruptible_sleep(kReconnectSec);
        }
    }
    state_ = 0;
}

bool SegmentRecorder::record_session(const std::string& url, int segment_sec) {
    AVFormatContext* in = avformat_alloc_context();
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "10000000", 0);  // 10 сек
    av_dict_set(&opts, "max_delay", "500000", 0);

    if (avformat_open_input(&in, url.c_str(), nullptr, &opts) < 0) {
        spdlog::warn("recorder: cannot open stream");
        av_dict_free(&opts);
        avformat_close_input(&in);
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        return false;
    }

    // Копируем видео + аудио (если есть, ТЗ §73.7). map: input stream -> output stream
    std::vector<int> map(in->nb_streams, -1);
    int out_count = 0;
    int video_idx = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const auto type = in->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) {
            if (type == AVMEDIA_TYPE_VIDEO && video_idx == -1) video_idx = static_cast<int>(i);
            map[i] = out_count++;
        }
    }
    if (video_idx == -1) {
        spdlog::warn("recorder: no video stream");
        avformat_close_input(&in);
        return false;
    }

    // ---- состояние текущего сегмента ----
    AVFormatContext* out = nullptr;
    std::string seg_rel_path;
    double seg_start_pts = -1;

    auto close_segment = [&]() {
        if (!out) return;
        av_write_trailer(out);
        if (out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
        out = nullptr;

        // обновляем metadata (ТЗ §36)
        try {
            const auto size = fs::file_size(fs::path(recordings_path_) / seg_rel_path);
            db_.tx([&](pqxx::work& w) {
                w.exec_params(
                    "UPDATE recordings SET ended_at=now(), file_size=$1, "
                    "duration = now() - started_at "
                    "WHERE file_path=$2 AND ended_at IS NULL",
                    static_cast<long long>(size), seg_rel_path);
            });
        } catch (const std::exception& e) {
            spdlog::error("recorder: db update failed: {}", e.what());
        }
        spdlog::info("segment closed: {}", seg_rel_path);
    };

    auto open_segment = [&]() -> bool {
        seg_rel_path = make_segment_path(recordings_path_);
        const std::string full = (fs::path(recordings_path_) / seg_rel_path).generic_string();

        if (avformat_alloc_output_context2(&out, nullptr, "mp4", full.c_str()) < 0 || !out) {
            spdlog::error("recorder: cannot create output context");
            return false;
        }

        for (unsigned i = 0; i < in->nb_streams; ++i) {
            if (map[i] < 0) continue;
            AVStream* os = avformat_new_stream(out, nullptr);
            if (!os) return false;
            avcodec_parameters_copy(os->codecpar, in->streams[i]->codecpar);
            os->codecpar->codec_tag = 0;
            os->time_base = in->streams[i]->time_base;
        }

        // фрагментированный MP4: файл читаем даже при обрыве записи
        av_opt_set(out->priv_data, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);

        if (avio_open(&out->pb, full.c_str(), AVIO_FLAG_WRITE) < 0 ||
            avformat_write_header(out, nullptr) < 0) {
            spdlog::error("recorder: cannot open segment file");
            if (out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
            return false;
        }

        seg_start_pts = -1;
        try {
            db_.tx([&](pqxx::work& w) {
                const auto* vpar = in->streams[video_idx]->codecpar;
                w.exec_params(
                    "INSERT INTO recordings (camera_id, started_at, file_path, codec, width, height) "
                    "VALUES (1, now(), $1, $2, $3, $4)",
                    seg_rel_path, avcodec_get_name(vpar->codec_id),
                    vpar->width, vpar->height);
            });
        } catch (const std::exception& e) {
            spdlog::error("recorder: db insert failed: {}", e.what());
        }
        spdlog::info("segment opened: {}", seg_rel_path);
        return true;
    };

    state_ = 1;
    if (event_fn_) event_fn_("recording.started", "{}");
    spdlog::info("recording started");

    bool stream_ok = true;
    AVPacket* pkt = av_packet_alloc();

    while (running_ && !reload_requested_) {
        if (av_read_frame(in, pkt) < 0) {
            spdlog::warn("recorder: stream read error");
            stream_ok = false;
            break;
        }

        const int si = pkt->stream_index;
        if (map[si] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        const bool is_video = (si == video_idx);
        const double pts_sec =
            pkt->pts == AV_NOPTS_VALUE
                ? -1
                : pkt->pts * av_q2d(in->streams[si]->time_base);

        if (!out && !open_segment()) {
            stream_ok = false;
            av_packet_unref(pkt);
            break;
        }

        if (is_video && pts_sec >= 0) {
            if (seg_start_pts < 0) seg_start_pts = pts_sec;

            // ротация: новый сегмент строго на кейфрейме
            const bool keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (keyframe && pts_sec - seg_start_pts >= segment_sec) {
                close_segment();
                if (!open_segment()) {
                    stream_ok = false;
                    av_packet_unref(pkt);
                    break;
                }
                seg_start_pts = pts_sec;
            }
        }

        pkt->stream_index = map[si];
        av_packet_rescale_ts(pkt, in->streams[si]->time_base,
                             out->streams[map[si]]->time_base);
        if (av_interleaved_write_frame(out, pkt) < 0)
            spdlog::warn("recorder: write frame failed");
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    close_segment();
    avformat_close_input(&in);
    state_ = 0;

    if (event_fn_) event_fn_("recording.stopped", "{}");
    return stream_ok;
}
