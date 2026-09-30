// Stream API (ТЗ §12-15): live HLS, архивные HLS-сессии, экспорт/скачивание MP4.
// Скачивание — через X-Accel-Redirect: nginx отдаёт файл с Range support (ТЗ §15).
#include "routes.hpp"

#include <spdlog/spdlog.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "../auth/session.hpp"
#include "../exporter/exporter.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

// Отдаёт файл из разрешённой директории (защита от path traversal)
crow::response serve_file(const std::string& dir, const std::string& file) {
    if (file.find("..") != std::string::npos || file.find('/') != std::string::npos)
        return crow::response(400);

    const auto path = std::filesystem::path(dir) / file;
    std::ifstream f(path, std::ios::binary);
    if (!f) return crow::response(404);

    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    crow::response res(200, body);

    if (file.ends_with(".m3u8")) {
        res.set_header("Content-Type", "application/vnd.apple.mpegurl");
        res.set_header("Cache-Control", "no-cache");  // live-плейлист не кэшируем
    } else if (file.ends_with(".ts")) {
        res.set_header("Content-Type", "video/mp2t");
    } else if (file.ends_with(".mp4") || file.ends_with(".m4s")) {
        res.set_header("Content-Type", "video/mp4");
    }
    return res;
}

std::string random_token() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(gen)));
    return buf;
}

}  // namespace

void register_stream_routes(crow::SimpleApp& app, db::Database& db, Exporter& exporter,
                            const std::string& live_path, const std::string& hls_path,
                            const std::string& recordings_path) {
    // GET /stream/live/<file> — live HLS (index.m3u8 + сегменты)
    CROW_ROUTE(app, "/stream/live/<string>")
    ([&db, &live_path](const crow::request& req, const std::string& file) {
        if (!auth::require_user(req, db)) return crow::response(401);
        return serve_file(live_path, file);
    });

    // GET /stream/archive/<token>/<file> — архивная HLS-сессия
    CROW_ROUTE(app, "/stream/archive/<string>/<string>")
    ([&db, &hls_path](const crow::request& req, const std::string& token, const std::string& file) {
        if (!auth::require_user(req, db)) return crow::response(401);
        if (token.find("..") != std::string::npos || token.find('/') != std::string::npos)
            return crow::response(400);
        return serve_file((std::filesystem::path(hls_path) / token).generic_string(), file);
    });

    // GET /api/recordings?date=YYYY-MM-DD — сегменты за день (для таймлайна архива)
    CROW_ROUTE(app, "/api/recordings")([&db](const crow::request& req) {
        if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");

        const char* date = req.url_params.get("date");
        if (!date) return json_error(400, "Укажите дату (ГГГГ-ММ-ДД)");
        const std::string d = date;
        if (d.size() != 10 || d[4] != '-' || d[7] != '-')
            return json_error(400, "Некорректный формат даты");

        try {
            return db.tx([&](pqxx::work& w) {
                const auto r = w.exec_params(
                    "SELECT id, started_at::text, ended_at::text, file_size "
                    "FROM recordings WHERE camera_id=1 AND started_at::date = $1::date "
                    "ORDER BY started_at",
                    d);
                std::vector<crow::json::wvalue> list;
                for (const auto& row : r) {
                    crow::json::wvalue seg;
                    seg["id"] = row["id"].as<long long>();
                    seg["started_at"] = row["started_at"].as<std::string>();
                    if (!row["ended_at"].is_null()) seg["ended_at"] = row["ended_at"].as<std::string>();
                    if (!row["file_size"].is_null()) seg["file_size"] = row["file_size"].as<long long>();
                    list.push_back(std::move(seg));
                }
                crow::json::wvalue res;
                res["recordings"] = std::move(list);
                return crow::response(200, res);
            });
        } catch (const std::exception& e) {
            spdlog::error("GET /api/recordings failed: {}", e.what());
            return json_error(500, "Внутренняя ошибка сервера");
        }
    });

    // POST /api/archive/session {start, end} — создаёт HLS-плейлист диапазона (ТЗ §73.2)
    // ISO 8601: "2026-09-30T18:30:00+03:00"
    CROW_ROUTE(app, "/api/archive/session").methods(crow::HTTPMethod::POST)(
        [&db, &hls_path, &recordings_path](const crow::request& req) {
            if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("start") || !body.has("end"))
                return json_error(400, "Укажите начало и конец периода");

            const std::string start = body["start"].s();
            const std::string end = body["end"].s();

            // устаревшие HLS-сессии чистит StorageManager (каждый цикл, TTL 2 часа)

            // сегменты диапазона + его длительность (для -t: активный сегмент
            // растёт на лету, без лимита ffmpeg читал бы его до закрытия)
            std::vector<std::string> segments;
            int duration_sec = 0;
            try {
                const auto res = db.tx([&](pqxx::work& w) {
                    const auto r = w.exec_params(
                        "SELECT file_path FROM recordings "
                        "WHERE camera_id=1 "
                        "  AND started_at < $2::timestamptz "
                        "  AND COALESCE(ended_at, now()) > $1::timestamptz "
                        "ORDER BY started_at",
                        start, end);
                    std::vector<std::string> out;
                    for (const auto& row : r) {
                        // пропускаем записи, чьи файлы уже удалены с диска —
                        // иначе ffmpeg упадёт на первом же отсутствующем сегменте
                        const auto p = std::string(row["file_path"].as<std::string>());
                        if (std::filesystem::exists(std::filesystem::path(recordings_path) / p))
                            out.push_back(p);
                        else
                            spdlog::warn("archive: файл сегмента отсутствует на диске, пропускаем: {}", p);
                    }
                    const auto d = w.exec_params(
                        "SELECT GREATEST(1, LEAST(86400, "
                        "EXTRACT(EPOCH FROM ($2::timestamptz - $1::timestamptz))::int))",
                        start, end);
                    return std::make_pair(std::move(out), d[0][0].as<int>());
                });
                segments = std::move(res.first);
                duration_sec = res.second;
            } catch (...) {
                return json_error(400, "Некорректный период времени");
            }
            if (segments.empty()) return json_error(404, "Записи за этот период не найдены");

            const std::string token = random_token();
            spdlog::info("archive session {}: {} сегментов, {} сек ({} .. {})",
                         token, segments.size(), duration_sec, start, end);
            const auto dir = std::filesystem::path(hls_path) / token;
            std::filesystem::create_directories(dir);

            const std::string list_file = (dir / "list.txt").generic_string();
            {
                std::ofstream f(list_file);
                for (const auto& seg : segments)
                    f << "file '" << (std::filesystem::path(recordings_path) / seg).generic_string() << "'\n";
            }

            // ffmpeg: concat -> HLS (stream copy); -t ограничивает длительность
            const std::string playlist = (dir / "index.m3u8").generic_string();
            const std::string dur = std::to_string(duration_sec);
            const pid_t pid = fork();
            if (pid < 0) {
                // fork не удался — waitpid(-1) перехватил бы чужой процесс
                spdlog::error("archive session {}: fork failed", token);
                std::filesystem::remove_all(dir);
                return json_error(500, "Не удалось подготовить запись");
            }
            if (pid == 0) {
                execlp("ffmpeg", "ffmpeg",
                       "-loglevel", "error", "-y",
                       "-f", "concat", "-safe", "0",
                       "-i", list_file.c_str(),
                       "-c", "copy",
                       "-t", dur.c_str(),
                       "-f", "hls", "-hls_time", "4",
                       "-hls_playlist_type", "vod",
                       playlist.c_str(),
                       static_cast<char*>(nullptr));
                _exit(127);
            }
            // ждём ffmpeg с потолком 60 сек: stream copy быстрый, но зависший
            // процесс не должен навсегда блокировать HTTP-поток
            int status = 0;
            bool reaped = false;
            for (int i = 0; i < 600; ++i) {
                if (waitpid(pid, &status, WNOHANG) > 0) { reaped = true; break; }
                usleep(100'000);
            }
            if (!reaped) {
                spdlog::error("archive session {}: ffmpeg завис, убиваем", token);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            std::filesystem::remove(list_file);

            if (status != 0 || !std::filesystem::exists(playlist)) {
                std::filesystem::remove_all(dir);
                spdlog::error("archive session {}: ffmpeg завершился с кодом {}", token, status);
                return json_error(500, "Не удалось подготовить запись");
            }
            spdlog::debug("archive session {}: плейлист готов", token);

            crow::json::wvalue res;
            res["url"] = "/stream/archive/" + token + "/index.m3u8";
            return crow::response(200, res);
        });

    // POST /api/export {start, end} — задача на скачивание MP4 (ТЗ §15)
    CROW_ROUTE(app, "/api/export").methods(crow::HTTPMethod::POST)(
        [&db, &exporter](const crow::request& req) {
            if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("start") || !body.has("end"))
                return json_error(400, "Укажите начало и конец периода");

            try {
                const long long id = exporter.create(body["start"].s(), body["end"].s());
                spdlog::info("export #{} создан: {} .. {}", id,
                             std::string(body["start"].s()), std::string(body["end"].s()));
                crow::json::wvalue res;
                res["id"] = id;
                res["status"] = "QUEUED";
                return crow::response(200, res);
            } catch (const std::invalid_argument&) {
                return json_error(400, "Некорректный период времени");
            }
        });

    // GET /api/export/:id — статус задачи
    CROW_ROUTE(app, "/api/export/<int>")
    ([&db, &exporter](const crow::request& req, int id) {
        if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");
        const auto info = exporter.get(id);
        if (!info) return json_error(404, "Не найдено");
        crow::json::wvalue res;
        res["id"] = info->id;
        res["status"] = info->status;
        res["start_time"] = info->start_time;
        res["end_time"] = info->end_time;
        if (info->status == "READY")
            res["download_url"] = "/api/export/" + std::to_string(info->id) + "/download";
        return crow::response(200, res);
    });

    // GET /api/export/:id/download — скачивание через nginx (X-Accel-Redirect).
    // nginx отдаёт файл с поддержкой Range Requests (ТЗ §15).
    CROW_ROUTE(app, "/api/export/<int>/download")
    ([&db, &exporter](const crow::request& req, int id) {
        if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");
        const auto info = exporter.get(id);
        if (!info || info->status != "READY" || info->file_name.empty())
            return json_error(404, "Файл ещё не готов");

        crow::response res(200);
        res.set_header("X-Accel-Redirect", "/internal/exports/" + info->file_name);
        res.set_header("Content-Type", "video/mp4");
        res.set_header("Content-Disposition",
                       "attachment; filename=\"homenvr_" + std::to_string(id) + ".mp4\"");
        return res;
    });
}

}  // namespace api
