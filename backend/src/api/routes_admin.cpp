// Admin API (ТЗ §40, §46-49): системные настройки и управление пользователями.
// Все endpoints — только ADMIN (ТЗ §7.2).
#include "routes.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

#include "../auth/password_hash.hpp"
#include "../auth/session.hpp"
#include "../motion/motion_detector.hpp"
#include "../recorder/segment_recorder.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

// ---------- Валидация значений настроек (H3 аудита) ----------
// Раньше значения не проверялись: segment_duration_sec=0 → сегмент на каждом
// кейфрейме + DB-флуд; max_storage_usage<=0 → очистка стирала весь архив.
enum class SettingKind { Number, Bool, Mode, Zones };

struct SettingRule {
    SettingKind kind;
    double min = 0, max = 0;
};

const std::unordered_map<std::string, SettingRule> kSettingsRules = {
    {"recording_mode",       {SettingKind::Mode}},
    {"segment_duration_sec", {SettingKind::Number, 60, 3600}},
    {"pre_buffer_sec",       {SettingKind::Number, 0, 60}},
    {"max_storage_usage",    {SettingKind::Number, 0.5, 0.95}},
    {"min_free_space",       {SettingKind::Number, 0.05, 0.5}},
    {"overwrite_enabled",    {SettingKind::Bool}},
    {"motion_enabled",       {SettingKind::Bool}},
    {"motion_sensitivity",   {SettingKind::Number, 0.0, 1.0}},
    {"motion_min_event_sec", {SettingKind::Number, 1, 300}},
    {"motion_cooldown_sec",  {SettingKind::Number, 0, 600}},
    {"motion_zones_detect",  {SettingKind::Zones}},
    {"motion_zones_ignore",  {SettingKind::Zones}},
};

// пустая строка = значение корректно; иначе — текст ошибки
std::string validate_setting(const std::string& key, const crow::json::rvalue& v) {
    const auto it = kSettingsRules.find(key);
    if (it == kSettingsRules.end()) return {};  // незнакомый ключ — отфильтруется whitelist'ом
    const auto& rule = it->second;

    // чтение с проверкой типа: .d()/.b()/.s() бросают при несоответствии
    try {
        switch (rule.kind) {
            case SettingKind::Number: {
                const double x = v.d();
                if (!std::isfinite(x) || x < rule.min || x > rule.max)
                    return key + ": ожидается число " + std::to_string(rule.min) +
                           "..." + std::to_string(rule.max);
                return {};
            }
            case SettingKind::Bool: {
                (void)v.b();  // бросит, если не boolean
                return {};
            }
            case SettingKind::Mode: {
                const auto mode = v.s();
                if (mode != "continuous" && mode != "motion")
                    return key + ": ожидается \"continuous\" или \"motion\"";
                return {};
            }
            case SettingKind::Zones: {
                auto parsed = nlohmann::json::parse(v.dump(), nullptr, false);
                if (parsed.is_discarded() || !parsed.is_array())
                    return key + ": ожидается JSON-массив зон";
                for (const auto& z : parsed) {
                    if (!z.is_object() || !z.contains("x") || !z.contains("y") ||
                        !z.contains("w") || !z.contains("h"))
                        return key + ": зона должна содержать x, y, w, h";
                    for (const char* f : {"x", "y", "w", "h"}) {
                        const double val = z[f].get<double>();
                        if (!std::isfinite(val) || val < 0 || val > 1)
                            return key + ": координаты зон — доли кадра 0..1";
                    }
                }
                return {};
            }
        }
    } catch (const std::exception&) {
        return key + ": неверный тип значения";
    }
    return {};
}

}  // namespace

void register_admin_routes(crow::SimpleApp& app, db::Database& db, MotionDetector& motion,
                           SegmentRecorder& recorder, const std::string& logs_path) {
    // GET /api/admin/settings — все настройки
    CROW_ROUTE(app, "/api/admin/settings")([&db](const crow::request& req) {
        if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");
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
            return json_error(500, "Внутренняя ошибка сервера");
        }
    });

    // PATCH /api/admin/settings {key: value, ...}
    CROW_ROUTE(app, "/api/admin/settings").methods(crow::HTTPMethod::PATCH)(
        [&db, &motion, &recorder](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "Некорректный JSON");

            // сначала валидируем ВСЕ значения (H3): один неверный параметр —
            // вся транзакция отклоняется, полуприменённых настроек не бывает
            std::string validation_error;
            for (const auto& key : body.keys()) {
                const std::string k = key;
                if (!kSettingsRules.count(k)) continue;  // незнакомый ключ — молча пропускаем
                validation_error = validate_setting(k, body[key]);
                if (!validation_error.empty()) break;
            }
            if (!validation_error.empty()) return json_error(400, validation_error);

            try {
                std::string changed;
                db.tx([&](pqxx::work& w) {
                    for (const auto& key : body.keys()) {
                        if (!kSettingsRules.count(key)) continue;  // whitelist по правилам
                        // сериализуем значение обратно в JSON для JSONB-колонки
                        std::string json_str = crow::json::wvalue(body[key]).dump();
                        w.exec_params(
                            "INSERT INTO system_settings (key, value, updated_at) "
                            "VALUES ($1, $2::jsonb, now()) "
                            "ON CONFLICT (key) DO UPDATE SET value=$2::jsonb, updated_at=now()",
                            key, json_str);
                        if (!changed.empty()) changed += ", ";
                        changed += std::string(key) + "=" + json_str;
                    }
                });
                if (!changed.empty()) spdlog::info("admin: настройки изменены: {}", changed);
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/admin/settings failed: {}", e.what());
                return json_error(500, "Внутренняя ошибка сервера");
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
        if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");
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
            return json_error(500, "Внутренняя ошибка сервера");
        }
    });

    // POST /api/admin/users {username, password, role}
    CROW_ROUTE(app, "/api/admin/users").methods(crow::HTTPMethod::POST)(
        [&db](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("username") || !body.has("password"))
                return json_error(400, "Укажите имя пользователя и пароль");

            const std::string username = [&] {
                try { return std::string(body["username"].s()); }
                catch (const std::exception&) { return std::string(); }
            }();
            const std::string role = [&] {
                try { return body.has("role") ? std::string(body["role"].s()) : std::string("USER"); }
                catch (const std::exception&) { return std::string(); }
            }();
            if (username.empty() || username.size() > 64)
                return json_error(400, "Некорректное имя пользователя");
            if (role != "USER" && role != "ADMIN")
                return json_error(400, "Некорректная роль");

            std::string hash;
            try {
                hash = auth::hash_password(body["password"].s());
            } catch (const std::invalid_argument& e) {
                return json_error(400, e.what());
            } catch (const std::exception&) {
                return json_error(400, "Неверный тип значения (password)");
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
            if (!admin) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "Некорректный JSON");

            // H5 аудита: доступ к JSON-полям только с проверкой типа —
            // исключение из .b()/.s() не должно покидать handler
            std::optional<bool> v_enabled;
            std::optional<std::string> v_password, v_role;
            try {
                if (body.has("enabled")) v_enabled = body["enabled"].b();
                if (body.has("password")) v_password = body["password"].s();
                if (body.has("role")) v_role = body["role"].s();
            } catch (const std::exception&) {
                return json_error(400, "Неверный тип значения (enabled/password/role)");
            }

            // админ не может отключить сам себя
            if (v_enabled.has_value() && !*v_enabled && admin->id == id)
                return json_error(400, "Нельзя отключить самого себя");

            // argon2-хеширование медленное (by design) — считаем ДО транзакции,
            // чтобы не держать блокировку БД сотни миллисекунд
            std::string new_hash;
            if (v_password.has_value()) {
                try {
                    new_hash = auth::hash_password(*v_password);
                } catch (const std::invalid_argument& e) {
                    return json_error(400, e.what());
                }
            }

            if (v_role.has_value() && *v_role != "USER" && *v_role != "ADMIN")
                return json_error(400, "Некорректная роль");

            try {
                db.tx([&](pqxx::work& w) {
                    if (v_password.has_value()) {
                        w.exec_params("UPDATE users SET password_hash=$1, updated_at=now() WHERE id=$2",
                                      new_hash, id);
                        // сбрасываем сессии пользователя после смены пароля
                        w.exec_params("DELETE FROM sessions WHERE user_id=$1", id);
                    }
                    if (v_enabled.has_value())
                        w.exec_params("UPDATE users SET enabled=$1, updated_at=now() WHERE id=$2",
                                      *v_enabled, id);
                    if (v_role.has_value())
                        w.exec_params(
                            "UPDATE users SET role_id=(SELECT id FROM roles WHERE name=$1), "
                            "updated_at=now() WHERE id=$2", *v_role, id);
                });
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/admin/users failed: {}", e.what());
                return json_error(500, "Внутренняя ошибка сервера");
            }

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // DELETE /api/admin/users/:id
    CROW_ROUTE(app, "/api/admin/users/<int>").methods(crow::HTTPMethod::DELETE)(
        [&db](const crow::request& req, int id) {
            const auto admin = auth::require_admin(req, db);
            if (!admin) return json_error(403, "Требуются права администратора");
            if (admin->id == id) return json_error(400, "Нельзя удалить самого себя");

            db.tx([&](pqxx::work& w) {
                w.exec_params("DELETE FROM users WHERE id=$1", id);
            });
            spdlog::info("user id={} deleted", id);

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // GET /api/admin/logs?lines=N — хвост файлового лога backend (отладка)
    CROW_ROUTE(app, "/api/admin/logs")([&db, &logs_path](const crow::request& req) {
        if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

        int want = 300;
        if (const char* p = req.url_params.get("lines")) {
            want = std::atoi(p);
            if (want < 10) want = 10;
            if (want > 2000) want = 2000;
        }

        const std::string path = logs_path + "/backend.log";
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return json_error(404, "Файл лога не найден");

        // читаем максимум последние 256 КБ — хвост лога
        const auto size = static_cast<std::streamoff>(f.tellg());
        const std::streamoff tail = std::min<std::streamoff>(size, 256 * 1024);
        f.seekg(size - tail);
        std::string buf(static_cast<size_t>(tail), '\0');
        f.read(buf.data(), tail);

        // режем на строки и берём последние want
        std::vector<std::string> lines;
        std::string cur;
        for (char c : buf) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) lines.push_back(std::move(cur));
        if (lines.size() > static_cast<size_t>(want))
            lines.erase(lines.begin(), lines.end() - want);

        crow::json::wvalue res;
        std::vector<crow::json::wvalue> out;
        out.reserve(lines.size());
        for (auto& l : lines) out.push_back(crow::json::wvalue(std::move(l)));
        res["lines"] = std::move(out);
        return crow::response(200, res);
    });
}

}  // namespace api
