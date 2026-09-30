#include "exporter.hpp"

#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace fs = std::filesystem;

Exporter::Exporter(db::Database& db, std::string recordings_path, std::string exports_path)
    : db_(db),
      recordings_path_(std::move(recordings_path)),
      exports_path_(std::move(exports_path)) {}

void Exporter::start() {
    if (running_.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(exports_path_, ec);
    thread_ = std::thread([this] { run(); });
    spdlog::info("exporter started");
}

void Exporter::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

long long Exporter::create(const std::string& start_iso, const std::string& end_iso) {
    // Валидация и создание задачи — приведение типов делает PostgreSQL
    try {
        return db_.tx([&](pqxx::work& w) {
            const auto r = w.exec_params(
                "INSERT INTO exports (camera_id, start_time, end_time, status, expires_at) "
                "VALUES (1, $1::timestamptz, $2::timestamptz, 'QUEUED', now() + interval '1 hour') "
                "RETURNING id",
                start_iso, end_iso);
            return r[0]["id"].as<long long>();
        });
    } catch (const std::exception& e) {
        throw std::invalid_argument("Некорректный период времени");
    }
}

std::optional<Exporter::ExportInfo> Exporter::get(long long id) {
    try {
        return db_.tx([&](pqxx::work& w) -> std::optional<ExportInfo> {
            const auto r = w.exec_params(
                "SELECT id, status, file_path, start_time::text, end_time::text "
                "FROM exports WHERE id=$1", id);
            if (r.empty()) return std::nullopt;
            ExportInfo info;
            info.id = r[0]["id"].as<long long>();
            info.status = r[0]["status"].as<std::string>();
            if (!r[0]["file_path"].is_null()) info.file_name = r[0]["file_path"].as<std::string>();
            info.start_time = r[0]["start_time"].as<std::string>();
            info.end_time = r[0]["end_time"].as<std::string>();
            return info;
        });
    } catch (...) {
        return std::nullopt;
    }
}

void Exporter::process(long long id, const std::string& start_iso, const std::string& end_iso) {
    auto set_status = [&](const char* status, const std::string& file = "") {
        db_.tx([&](pqxx::work& w) {
            if (file.empty())
                w.exec_params("UPDATE exports SET status=$1 WHERE id=$2", status, id);
            else
                w.exec_params("UPDATE exports SET status=$1, file_path=$2 WHERE id=$3",
                              status, file, id);
        });
    };

    try {
        set_status("PROCESSING");

        // 1. Находим сегменты, пересекающие диапазон (ТЗ §16);
        //    активный (ещё пишущийся) сегмент тоже включаем — fMP4 читаем на лету
        std::vector<std::string> segments;
        int duration_sec = 0;
        {
            const auto res = db_.tx([&](pqxx::work& w) {
                const auto r = w.exec_params(
                    "SELECT file_path FROM recordings "
                    "WHERE camera_id=1 "
                    "  AND started_at < $2::timestamptz "
                    "  AND COALESCE(ended_at, now()) > $1::timestamptz "
                    "ORDER BY started_at",
                    start_iso, end_iso);
                std::vector<std::string> out;
                for (const auto& row : r) out.push_back(row["file_path"].as<std::string>());
                const auto d = w.exec_params(
                    "SELECT GREATEST(1, LEAST(86400, "
                    "EXTRACT(EPOCH FROM ($2::timestamptz - $1::timestamptz))::int))",
                    start_iso, end_iso);
                return std::make_pair(std::move(out), d[0][0].as<int>());
            });
            segments = std::move(res.first);
            duration_sec = res.second;
        }

        if (segments.empty()) {
            spdlog::warn("export {}: no segments in range", id);
            set_status("FAILED");
            return;
        }

        // 2. Список для concat demuxer
        const std::string list_file = exports_path_ + "/list_" + std::to_string(id) + ".txt";
        {
            std::ofstream f(list_file);
            for (const auto& seg : segments)
                f << "file '" << (fs::path(recordings_path_) / seg).generic_string() << "'\n";
        }

        // 3-5. ffmpeg: concat + stream copy → MP4 (ТЗ §15)
        const std::string out_name = "export_" + std::to_string(id) + ".mp4";
        const std::string out_path = (fs::path(exports_path_) / out_name).generic_string();

        const pid_t pid = fork();
        if (pid == 0) {
            const std::string dur = std::to_string(duration_sec);
            execlp("ffmpeg", "ffmpeg",
                   "-loglevel", "error", "-y",
                   "-f", "concat", "-safe", "0",
                   "-i", list_file.c_str(),
                   "-c", "copy",
                   "-t", dur.c_str(),
                   "-movflags", "+faststart",
                   out_path.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        fs::remove(list_file);

        // 6. Готово
        if (status == 0 && fs::exists(out_path)) {
            set_status("READY", out_name);
            spdlog::info("export {} ready ({} segments)", id, segments.size());
        } else {
            spdlog::error("export {}: ffmpeg failed (status {})", id, status);
            set_status("FAILED");
        }
    } catch (const std::exception& e) {
        spdlog::error("export {} failed: {}", id, e.what());
        try { set_status("FAILED"); } catch (...) {}
    }
}

void Exporter::cleanup_expired() {
    try {
        const auto expired = db_.tx([](pqxx::work& w) {
            const auto r = w.exec(
                "UPDATE exports SET status='EXPIRED' "
                "WHERE status='READY' AND expires_at < now() "
                "RETURNING file_path");
            std::vector<std::string> files;
            for (const auto& row : r)
                if (!row["file_path"].is_null()) files.push_back(row["file_path"].as<std::string>());
            return files;
        });
        for (const auto& f : expired) {
            std::error_code ec;
            fs::remove(fs::path(exports_path_) / f, ec);
        }
    } catch (const std::exception& e) {
        spdlog::error("export cleanup failed: {}", e.what());
    }
}

void Exporter::run() {
    while (running_) {
        // берём одну задачу из очереди
        auto task = db_.tx([](pqxx::work& w) -> std::optional<std::tuple<long long, std::string, std::string>> {
            const auto r = w.exec(
                "SELECT id, start_time::text, end_time::text FROM exports "
                "WHERE status='QUEUED' ORDER BY created_at LIMIT 1");
            if (r.empty()) return std::nullopt;
            return std::make_tuple(r[0]["id"].as<long long>(),
                                   r[0]["start_time"].as<std::string>(),
                                   r[0]["end_time"].as<std::string>());
        });

        if (task) {
            auto [id, start, end] = *task;
            process(id, start, end);
        } else {
            cleanup_expired();
            for (int i = 0; i < 20 && running_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
