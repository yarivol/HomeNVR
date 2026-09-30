// Events API (ТЗ §24, §40): список событий движения + thumbnails.
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

#include "../auth/session.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

}  // namespace

void register_events_routes(crow::SimpleApp& app, db::Database& db,
                            const std::string& thumbnails_path) {
    // GET /api/events?date=YYYY-MM-DD — события за день (без date — последние 50)
    CROW_ROUTE(app, "/api/events")([&db](const crow::request& req) {
        if (!auth::require_user(req, db)) return json_error(401, "unauthorized");

        const char* date = req.url_params.get("date");
        try {
            return db.tx([&](pqxx::work& w) {
                pqxx::result r;
                if (date) {
                    // строгая валидация формата даты
                    const std::string d = date;
                    if (d.size() != 10 || d[4] != '-' || d[7] != '-')
                        return crow::response(400, "invalid date format");
                    r = w.exec_params(
                        "SELECT id, started_at::text, ended_at::text, motion_score, thumbnail_path "
                        "FROM motion_events WHERE camera_id=1 AND started_at::date = $1::date "
                        "ORDER BY started_at DESC",
                        d);
                } else {
                    r = w.exec(
                        "SELECT id, started_at::text, ended_at::text, motion_score, thumbnail_path "
                        "FROM motion_events WHERE camera_id=1 "
                        "ORDER BY started_at DESC LIMIT 50");
                }

                std::vector<crow::json::wvalue> list;
                for (const auto& row : r) {
                    crow::json::wvalue e;
                    e["id"] = row["id"].as<long long>();
                    e["started_at"] = row["started_at"].as<std::string>();
                    if (!row["ended_at"].is_null()) e["ended_at"] = row["ended_at"].as<std::string>();
                    if (!row["motion_score"].is_null()) e["score"] = row["motion_score"].as<double>();
                    if (!row["thumbnail_path"].is_null())
                        e["thumbnail"] = "/api/thumbnails/" + row["thumbnail_path"].as<std::string>();
                    list.push_back(std::move(e));
                }
                crow::json::wvalue res;
                res["events"] = std::move(list);
                return crow::response(200, res);
            });
        } catch (const std::exception& e) {
            spdlog::error("GET /api/events failed: {}", e.what());
            return json_error(500, "internal error");
        }
    });

    // GET /api/thumbnails/<file> — JPEG события (авторизация обязательна)
    CROW_ROUTE(app, "/api/thumbnails/<string>")
    ([&db, &thumbnails_path](const crow::request& req, const std::string& file) {
        if (!auth::require_user(req, db)) return crow::response(401);

        // защита от path traversal
        if (file.find("..") != std::string::npos || file.find('/') != std::string::npos)
            return crow::response(400);

        const auto path = std::filesystem::path(thumbnails_path) / file;
        std::ifstream f(path, std::ios::binary);
        if (!f) return crow::response(404);

        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        crow::response res(200, body);
        res.set_header("Content-Type", "image/jpeg");
        res.set_header("Cache-Control", "private, max-age=3600");
        return res;
    });
}

}  // namespace api
