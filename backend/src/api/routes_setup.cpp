// Setup wizard (ТЗ §73.5): доступен ТОЛЬКО пока в системе нет ни одного пользователя.
// После создания админа wizard закрывается навсегда.
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>

#include "../auth/password_hash.hpp"
#include "../common/crypto.hpp"
#include "../recorder/segment_recorder.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

bool has_users(db::Database& db) {
    return db.tx([](pqxx::work& w) {
        return !w.exec("SELECT 1 FROM users LIMIT 1").empty();
    });
}

// L6 аудита: RTSP URL из админки уходит в ffmpeg/libav — допускаем только
// rtsp:// и rtsps:// (иначе admin может подсунуть file:/http:/unix:)
bool valid_rtsp_url(const std::string& u) {
    return u.rfind("rtsp://", 0) == 0 || u.rfind("rtsps://", 0) == 0;
}

}  // namespace

void register_setup_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                           CameraManager& cam, SegmentRecorder& recorder) {
    // GET /api/setup/status — нужен frontend'у, чтобы показать wizard
    CROW_ROUTE(app, "/api/setup/status")([&db] {
        crow::json::wvalue res;
        res["completed"] = has_users(db);
        return crow::response(200, res);
    });

    // POST /api/setup {username, password, camera: {name, ip, onvif_port, username, password, rtsp_url, rtsp_sub_url}}
    CROW_ROUTE(app, "/api/setup").methods(crow::HTTPMethod::POST)(
        [&db, &key_hex, &cam, &recorder](const crow::request& req) {
            if (has_users(db))
                return json_error(403, "Setup already completed");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("username") || !body.has("password"))
                return json_error(400, "Укажите имя пользователя и пароль");

            const std::string username = [&] {
                try { return std::string(body["username"].s()); }
                catch (const std::exception&) { return std::string(); }
            }();
            const std::string password = [&] {
                try { return std::string(body["password"].s()); }
                catch (const std::exception&) { return std::string(); }
            }();
            if (username.empty() || username.size() > 64)
                return json_error(400, "Некорректное имя пользователя");

            // H5: все обращения к вложенным полям camera — только через
            // проверяемые чтения, исключение из .s()/.i() не покидает handler
            std::optional<std::string> c_name, c_ip, c_user, c_pass, c_rtsp, c_rtsp_sub;
            std::optional<int> c_port;
            try {
                if (body.has("camera")) {
                    const auto& c = body["camera"];
                    c_name  = std::string(c["name"].s());
                    c_ip    = std::string(c["ip"].s());
                    c_port  = static_cast<int>(c["onvif_port"].i());
                    c_user  = std::string(c["username"].s());
                    c_pass  = std::string(c["password"].s());
                    if (c.has("rtsp_url") && !std::string(c["rtsp_url"].s()).empty()) {
                        c_rtsp = std::string(c["rtsp_url"].s());
                        if (!valid_rtsp_url(*c_rtsp))
                            return json_error(400, "RTSP URL должен начинаться с rtsp://");
                    }
                    if (c.has("rtsp_sub_url") && !std::string(c["rtsp_sub_url"].s()).empty()) {
                        c_rtsp_sub = std::string(c["rtsp_sub_url"].s());
                        if (!valid_rtsp_url(*c_rtsp_sub))
                            return json_error(400, "RTSP URL должен начинаться с rtsp://");
                    }
                }
            } catch (const std::exception&) {
                return json_error(400, "Некорректные данные камеры");
            }

            std::string hash;
            try {
                hash = auth::hash_password(password);
            } catch (const std::invalid_argument& e) {
                return json_error(400, e.what());
            } catch (const std::exception&) {
                return json_error(400, "Некорректный пароль");
            }

            try {
                db.tx([&](pqxx::work& w) {
                    const auto role = w.exec("SELECT id FROM roles WHERE name = 'ADMIN'");
                    w.exec_params("INSERT INTO users (username, password_hash, role_id) VALUES ($1, $2, $3)",
                                  username, hash, role[0]["id"].as<int>());

                    // Камера — опционально на шаге wizard
                    if (c_name.has_value()) {
                        w.exec_params(
                            "INSERT INTO camera (id, name, ip_address, onvif_port, username, password_encrypted) "
                            "VALUES (1, $1, $2, $3, $4, $5) "
                            "ON CONFLICT (id) DO UPDATE SET name=$1, ip_address=$2, onvif_port=$3, "
                            "username=$4, password_encrypted=$5, updated_at=now()",
                            *c_name, *c_ip, c_port.value_or(80), *c_user,
                            crypto::encrypt(*c_pass, key_hex));

                        // RTSP URL'ы — зашифрованы, наружу не отдаются (ТЗ §39)
                        if (c_rtsp.has_value())
                            w.exec_params(
                                "INSERT INTO camera_profiles (camera_id, profile_name, is_substream, rtsp_url_encrypted) "
                                "VALUES (1, $1, false, $2) "
                                "ON CONFLICT DO NOTHING",
                                "main", crypto::encrypt(*c_rtsp, key_hex));
                        if (c_rtsp_sub.has_value())
                            w.exec_params(
                                "INSERT INTO camera_profiles (camera_id, profile_name, is_substream, rtsp_url_encrypted) "
                                "VALUES (1, $1, true, $2) "
                                "ON CONFLICT DO NOTHING",
                                "sub", crypto::encrypt(*c_rtsp_sub, key_hex));
                    }
                    w.exec("UPDATE system_settings SET value = 'true', updated_at = now() "
                           "WHERE key = 'setup_completed'");
                });
            } catch (const std::exception& e) {
                spdlog::error("POST /api/setup failed: {}", e.what());
                return json_error(500, "Внутренняя ошибка сервера");
            }

            spdlog::info("setup completed, admin '{}' created", username);
            cam.reload();
            recorder.reload();

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });
}

}  // namespace api
