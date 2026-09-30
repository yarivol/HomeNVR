#include "motion_detector.hpp"

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <deque>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace {
constexpr int kReconnectSec = 10;
constexpr int kAnalysisWidth = 320;    // анализ на уменьшенном кадре (ТЗ §60)
constexpr int kAnalysisHeight = 180;

// Зона: прямоугольник в долях кадра (0..1)
struct Zone {
    double x, y, w, h;
    cv::Rect to_rect(int frame_w, int frame_h) const {
        return {static_cast<int>(x * frame_w), static_cast<int>(y * frame_h),
                static_cast<int>(w * frame_w), static_cast<int>(h * frame_h)};
    }
};

std::vector<Zone> parse_zones(const std::string& json) {
    std::vector<Zone> out;
    try {
        for (const auto& z : nlohmann::json::parse(json)) {
            out.push_back({z.value("x", 0.0), z.value("y", 0.0),
                           z.value("w", 1.0), z.value("h", 1.0)});
        }
    } catch (...) {
        // битая настройка — игнорируем, работаем без зон
    }
    return out;
}
}  // namespace

MotionDetector::MotionDetector(db::Database& db, std::string thumbnails_path)
    : db_(db), thumbnails_path_(std::move(thumbnails_path)) {}

void MotionDetector::start() {
    if (running_.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(thumbnails_path_, ec);
    thread_ = std::thread([this] { run(); });
    spdlog::info("motion detector started");
}

void MotionDetector::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MotionDetector::reload() { reload_requested_ = true; }

void MotionDetector::interruptible_sleep(int seconds) {
    for (int i = 0; i < seconds * 10 && running_ && !reload_requested_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

bool MotionDetector::enabled() {
    try {
        return db_.tx([](pqxx::work& w) {
            return w.exec("SELECT value::text FROM system_settings WHERE key='motion_enabled'")[0][0]
                       .as<std::string>() == "true";
        });
    } catch (...) {
        return true;
    }
}

double MotionDetector::sensitivity() {
    try {
        return db_.tx([](pqxx::work& w) {
            return std::stod(w.exec(
                "SELECT value::text FROM system_settings WHERE key='motion_sensitivity'")[0][0]
                                 .as<std::string>());
        });
    } catch (...) {
        return 0.5;
    }
}

int MotionDetector::min_event_sec() {
    try {
        return db_.tx([](pqxx::work& w) {
            return std::stoi(w.exec(
                "SELECT value::text FROM system_settings WHERE key='motion_min_event_sec'")[0][0]
                                 .as<std::string>());
        });
    } catch (...) {
        return 3;
    }
}

int MotionDetector::cooldown_sec() {
    try {
        return db_.tx([](pqxx::work& w) {
            return std::stoi(w.exec(
                "SELECT value::text FROM system_settings WHERE key='motion_cooldown_sec'")[0][0]
                                 .as<std::string>());
        });
    } catch (...) {
        return 10;
    }
}

std::string MotionDetector::zones_json(const char* key) {
    try {
        return db_.tx([&](pqxx::work& w) {
            const auto r = w.exec_params("SELECT value::text FROM system_settings WHERE key=$1", key);
            return r.empty() ? std::string("[]") : r[0][0].as<std::string>();
        });
    } catch (...) {
        return "[]";
    }
}

std::string MotionDetector::current_url() {
    std::lock_guard lock(mutex_);
    if (!url_provider_) return "";
    try {
        return url_provider_();
    } catch (const std::exception& e) {
        spdlog::error("motion url provider failed: {}", e.what());
        return "";
    }
}

void MotionDetector::save_event(double score, const std::string& thumbnail_file) {
    try {
        db_.tx([&](pqxx::work& w) {
            // привязываем к текущему записываемому сегменту
            w.exec_params(
                "INSERT INTO motion_events (camera_id, recording_id, started_at, ended_at, "
                "duration, motion_score, thumbnail_path) "
                "VALUES (1, "
                "  (SELECT id FROM recordings WHERE ended_at IS NULL ORDER BY started_at DESC LIMIT 1), "
                "  now() - make_interval(secs => $2), now(), "
                "  make_interval(secs => $2), $1, $3)",
                score, min_event_sec(), thumbnail_file);
        });
    } catch (const std::exception& e) {
        spdlog::error("motion event save failed: {}", e.what());
    }
}

void MotionDetector::run() {
    while (running_) {
        reload_requested_ = false;

        if (!enabled()) {
            interruptible_sleep(5);
            continue;
        }
        const std::string url = current_url();
        if (url.empty()) {
            interruptible_sleep(5);
            continue;
        }

        if (!detection_session(url)) interruptible_sleep(kReconnectSec);
    }
}

bool MotionDetector::detection_session(const std::string& url) {
    AVFormatContext* in = avformat_alloc_context();
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "10000000", 0);

    if (avformat_open_input(&in, url.c_str(), nullptr, &opts) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
        spdlog::warn("motion: cannot open stream");
        av_dict_free(&opts);
        avformat_close_input(&in);
        return false;
    }
    av_dict_free(&opts);

    int video_idx = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i)
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_idx == -1)
            video_idx = static_cast<int>(i);
    if (video_idx == -1) {
        avformat_close_input(&in);
        return false;
    }

    const auto* codec = avcodec_find_decoder(in->streams[video_idx]->codecpar->codec_id);
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!codec || !dec ||
        avcodec_parameters_to_context(dec, in->streams[video_idx]->codecpar) < 0 ||
        avcodec_open2(dec, codec, nullptr) < 0) {
        spdlog::error("motion: decoder init failed");
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&in);
        return false;
    }

    SwsContext* sws = sws_getContext(
        dec->width, dec->height, dec->pix_fmt,
        kAnalysisWidth, kAnalysisHeight, AV_PIX_FMT_GRAY8,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&dec);
        avformat_close_input(&in);
        return false;
    }

    spdlog::info("motion detection running");

    // Состояние детектора
    cv::Mat prev_frame;
    cv::Mat best_frame;             // кадр с максимальным движением для thumbnail
    bool in_event = false;
    double event_max_score = 0;
    auto event_start = std::chrono::steady_clock::now();
    auto last_event_end = std::chrono::steady_clock::now() - std::chrono::hours(1);
    int frame_counter = 0;

    // Зоны перечитываются раз в ~5 сек (примерно каждые 100 кадров)
    std::vector<Zone> detect_zones = parse_zones(zones_json("motion_zones_detect"));
    std::vector<Zone> ignore_zones = parse_zones(zones_json("motion_zones_ignore"));

    // Порог по sensitivity (ТЗ §22): 1.0 = самый чувствительный
    const double sens = sensitivity();
    const int diff_threshold = static_cast<int>(35 - sens * 25);        // 10..35
    const double min_area_ratio = 0.02 - sens * 0.015;                  // 0.005..0.02 кадра

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool stream_ok = true;

    while (running_ && !reload_requested_) {
        if (av_read_frame(in, pkt) < 0) {
            spdlog::warn("motion: stream read error");
            stream_ok = false;
            break;
        }
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(dec, frame) == 0) {
            // анализируем каждый 3-й кадр — достаточно для детекции, экономит CPU
            if (++frame_counter % 3 != 0) continue;

            // периодически перечитываем зоны (админ мог поменять)
            if (frame_counter % 300 == 0) {
                detect_zones = parse_zones(zones_json("motion_zones_detect"));
                ignore_zones = parse_zones(zones_json("motion_zones_ignore"));
            }

            // --- пайплайн ТЗ §21: resize → gray → blur → diff → threshold → contours ---
            cv::Mat gray(kAnalysisHeight, kAnalysisWidth, CV_8UC1);
            auto* dst = gray.data;
            int dst_stride = gray.step;
            sws_scale(sws, frame->data, frame->linesize, 0, dec->height, &dst, &dst_stride);
            cv::GaussianBlur(gray, gray, {5, 5}, 0);

            if (prev_frame.empty()) {
                prev_frame = gray.clone();
                continue;
            }

            cv::Mat diff, thresh;
            cv::absdiff(gray, prev_frame, diff);
            prev_frame = gray;
            cv::threshold(diff, thresh, diff_threshold, 255, cv::THRESH_BINARY);
            cv::morphologyEx(thresh, thresh, cv::MORPH_OPEN,
                             cv::getStructuringElement(cv::MORPH_RECT, {3, 3}));

            // зоны ignore — зануляем
            for (const auto& z : ignore_zones)
                cv::rectangle(thresh, z.to_rect(kAnalysisWidth, kAnalysisHeight), 0, cv::FILLED);

            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            double motion_area = 0;
            const double min_area = min_area_ratio * kAnalysisWidth * kAnalysisHeight;
            for (const auto& c : contours) {
                const double area = cv::contourArea(c);
                if (area < min_area) continue;
                const cv::Rect box = cv::boundingRect(c);
                // если заданы зоны детекции — контур должен пересекать хотя бы одну
                if (!detect_zones.empty()) {
                    bool inside = false;
                    for (const auto& z : detect_zones)
                        if ((box & z.to_rect(kAnalysisWidth, kAnalysisHeight)).area() > 0) {
                            inside = true;
                            break;
                        }
                    if (!inside) continue;
                }
                motion_area += area;
            }

            const double score = motion_area / (kAnalysisWidth * kAnalysisHeight);  // 0..1
            const auto now = std::chrono::steady_clock::now();
            const auto since_last = std::chrono::duration<double>(now - last_event_end).count();

            if (score > min_area_ratio && !in_event && since_last >= cooldown_sec()) {
                in_event = true;
                event_start = now;
                event_max_score = score;
                best_frame = gray.clone();
                if (event_fn_) event_fn_("motion.started", "{}");
                spdlog::info("motion detected, score {:.3f}", score);
            } else if (in_event) {
                if (score > event_max_score) {
                    event_max_score = score;
                    best_frame = gray.clone();
                }
                // событие заканчивается после 2 секунд тишины
                if (score <= min_area_ratio * 0.3) {
                    const double elapsed = std::chrono::duration<double>(now - event_start).count();
                    // 2 секунды без движения И событие длиннее минимума → завершаем
                    if (elapsed >= min_event_sec()) {
                        in_event = false;
                        last_event_end = now;

                        // thumbnail (ТЗ §26): маленький JPEG
                        std::string thumb_file;
                        const std::string name =
                            "event_" +
                            std::to_string(
                                std::chrono::system_clock::now().time_since_epoch().count()) +
                            ".jpg";
                        if (cv::imwrite((fs::path(thumbnails_path_) / name).generic_string(),
                                        best_frame, {cv::IMWRITE_JPEG_QUALITY, 70}))
                            thumb_file = name;

                        save_event(event_max_score, thumb_file);
                        if (event_fn_)
                            event_fn_("motion.ended",
                                      "{\"score\":" + std::to_string(event_max_score) + "}");
                        spdlog::info("motion ended, max score {:.3f}", event_max_score);
                    }
                } else {
                    event_start = now;  // движение продолжается — сдвигаем таймер тишины
                }
            }
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    sws_freeContext(sws);
    avcodec_free_context(&dec);
    avformat_close_input(&in);
    return stream_ok;
}
