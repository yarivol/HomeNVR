// Admin API (ТЗ §40, §46-49): системные настройки и управление пользователями.
// Все endpoints — только ADMIN (ТЗ §7.2).
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include "../auth/password_hash.hpp"
#include "../auth/session.hpp"
#include "../motion/motion_detector.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

// Разрешённые ключи настроек (whitelist — никаких произвольных ключей)
const std::unordered_map<std::string, bool> kAllowedSettings = {
    {"recording_mode", true}, {"segment_duration_sec", true}, {"pre_buffer_sec", true},
    {"max_storage_usage", true}, {"min_free_space", true}, {"overwrite_enabled", true},
    {"motion_enabled", true}, {"motion_sensitivity", true}, {"motion_min_event_sec", true},
    {"motion_cooldown_sec", true}, {"motion_zones_detect", true}, {"motion_zones_ignore", true},
};

}  // namespace

void register_admin_routes(crow::SimpleApp& app, db::Database& db, MotionDetector& motion,
                           SegmentRecorder& recorder) {
    // GET /api/admin/settings — все настройки
    CROW_ROUTE(app, "/api/admin/settings")([&db](const crow::request& req) {
        if (!auth::require_admin(req, db)) return json_error(403, "admin only");
        try {
            return db.tx([](pqxx::work& w) {
                const auto r = w.exec("SELECT key, value::text FROM system_settings");
                crow::json::wvalue res;
                for (const auto& row : r) {
                    // значения хранятся как JSONB — отдаём как есть
                    res[row["key"].as<std::string>()] =
                        crow::json::load(row["value"].as<std::string>());
                }
                return crow::response(200, res);
            });
        } catch (const std::exception& e) {
            spdlog::error("GET /api/admin/settings failed: {}", e.what());
            return json_error(500, "internal error");
        }
    });

    // PATCH /api/admin/settings {key: value, ...}
    CROW_ROUTE(app, "/api/admin/settings").methods(crow::HTTPMethod::PATCH)(
        [&db, &motion, &recorder](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "admin only");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "invalid json");

            try {
                db.tx([&](pqxx::work& w) {
                    for (const auto& key : body.keys()) {
                        if (!kAllowedSettings.count(key)) continue;  // незнакомый ключ — молча пропускаем
                        // сериализуем значение обратно в JSON для JSONB-колонки
                        std::string json_str = crow::json::wvalue(body[key]).dump();
                        w.exec_params(
                            "INSERT INTO system_settings (key, value, updated_at) "
                            "VALUES ($1, $2::jsonb, now()) "
                            "ON CONFLICT (key) DO UPDATE SET value=$2::jsonb, updated_at=now()",
                            key, json_str);
                    }
                });
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/admin/settings failed: {}", e.what());
                return json_error(500, "internal error");
            }

            // настройки записи/motion применяются на лету
            motion.reload();
            recorder.reload();

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // GET /api/admin/users — список пользователей (без хешей!)
    CROW_ROUTE(app, "/api/admin/users")([&db](const crow::request& req) {
        if (!auth::require_admin(req, db)) return json_error(403, "admin only");
        try {
            return db.tx([](pqxx::work& w) {
                const auto r = w.exec(
                    "SELECT u.id, u.username, r.name AS role, u.enabled, u.created_at::text "
                    "FROM users u JOIN roles r ON r.id = u.role_id ORDER BY u.id");
                std::vector<crow::json::wvalue> list;
                for (const auto& row : r) {
                    crow::json::wvalue u;
                    u["id"] = row["id"].as<int>();
                    u["username"] = row["username"].as<std::string>();
                    u["role"] = row["role"].as<std::string>();
                    u["enabled"] = row["enabled"].as<bool>();
                    u["created_at"] = row["created_at"].as<std::string>();
                    list.push_back(std::move(u));
                }
                crow::json::wvalue res;
                res["users"] = std::move(list);
                return crow::response(200, res);
            });
        } catch (const std::exception& e) {
            spdlog::error("GET /api/admin/users failed: {}", e.what());
            return json_error(500, "internal error");
        }
    });

    // POST /api/admin/users {username, password, role}
    CROW_ROUTE(app, "/api/admin/users").methods(crow::HTTPMethod::POST)(
        [&db](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "admin only");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("username") || !body.has("password"))
                return json_error(400, "username and password required");

            const std::string username = body["username"].s();
            const std::string role = body.has("role") ? std::string(body["role"].s()) : "USER";
            if (username.empty() || username.size() > 64)
                return json_error(400, "invalid username");
            if (role != "USER" && role != "ADMIN")
                return json_error(400, "invalid role");

            std::string hash;
            try {
                hash = auth::hash_password(body["password"].s());
            } catch (const std::invalid_argument& e) {
                return json_error(400, e.what());
            }

            try {
                db.tx([&](pqxx::work& w) {
                    const auto r = w.exec_params("SELECT id FROM roles WHERE name=$1", role);
                    w.exec_params(
                        "INSERT INTO users (username, password_hash, role_id) VALUES ($1, $2, $3)",
                        username, hash, r[0]["id"].as<int>());
                });
            } catch (const pqxx::unique_violation&) {
                return json_error(409, "Пользователь уже существует");
            }

            spdlog::info("user '{}' created", username);
            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // PATCH /api/admin/users/:id {password?, enabled?, role?}
    CROW_ROUTE(app, "/api/admin/users/<int>").methods(crow::HTTPMethod::PATCH)(
        [&db](const crow::request& req, int id) {
            const auto admin = auth::require_admin(req, db);
            if (!admin) return json_error(403, "admin only");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "invalid json");

            // админ не может отключить сам себя
            if (body.has("enabled") && !static_cast<bool>(body["enabled"].b()) && admin->id == id)
                return json_error(400, "Нельзя отключить самого себя");

            try {
                db.tx([&](pqxx::work& w) {
                    if (body.has("password")) {
                        w.exec_params("UPDATE users SET password_hash=$1, updated_at=now() WHERE id=$2",
                                      auth::hash_password(body["password"].s()), id);
                        // сбрасываем сессии пользователя после смены пароля
                        w.exec_params("DELETE FROM sessions WHERE user_id=$1", id);
                    }
                    if (body.has("enabled"))
                        w.exec_params("UPDATE users SET enabled=$1, updated_at=now() WHERE id=$2",
                                      static_cast<bool>(body["enabled"].b()), id);
                    if (body.has("role")) {
                        const std::string role = body["role"].s();
                        if (role != "USER" && role != "ADMIN") throw std::invalid_argument("role");
                        w.exec_params(
                            "UPDATE users SET role_id=(SELECT id FROM roles WHERE name=$1), "
                            "updated_at=now() WHERE id=$2", role, id);
                    }
                });
            } catch (const std::invalid_argument& e) {
                return json_error(400, e.what());
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/admin/users failed: {}", e.what());
                return json_error(500, "internal error");
            }

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // DELETE /api/admin/users/:id
    CROW_ROUTE(app, "/api/admin/users/<int>").methods(crow::HTTPMethod::DELETE)(
        [&db](const crow::request& req, int id) {
            const auto admin = auth::require_admin(req, db);
            if (!admin) return json_error(403, "admin only");
            if (admin->id == id) return json_error(400, "Нельзя удалить самого себя");

            db.tx([&](pqxx::work& w) {
                w.exec_params("DELETE FROM users WHERE id=$1", id);
            });
            spdlog::info("user id={} deleted", id);

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });
}

}  // namespace api
