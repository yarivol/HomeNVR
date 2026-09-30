// Setup wizard (ТЗ §73.5): доступен ТОЛЬКО пока в системе нет ни одного пользователя.
// После создания админа wizard закрывается навсегда.
#include "routes.hpp"

#include <spdlog/spdlog.h>

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

            const std::string username = body["username"].s();
            const std::string password = body["password"].s();
            if (username.empty() || username.size() > 64)
                return json_error(400, "Некорректное имя пользователя");

            std::string hash;
            try {
                hash = auth::hash_password(password);
            } catch (const std::invalid_argument& e) {
                return json_error(400, e.what());
            }

            db.tx([&](pqxx::work& w) {
                const auto role = w.exec("SELECT id FROM roles WHERE name = 'ADMIN'");
                w.exec_params("INSERT INTO users (username, password_hash, role_id) VALUES ($1, $2, $3)",
                              username, hash, role[0]["id"].as<int>());

                // Камера — опционально на шаге wizard
                if (body.has("camera")) {
                    const auto& c = body["camera"];
                    w.exec_params(
                        "INSERT INTO camera (id, name, ip_address, onvif_port, username, password_encrypted) "
                        "VALUES (1, $1, $2, $3, $4, $5) "
                        "ON CONFLICT (id) DO UPDATE SET name=$1, ip_address=$2, onvif_port=$3, "
                        "username=$4, password_encrypted=$5, updated_at=now()",
                        std::string(c["name"].s()),
                        std::string(c["ip"].s()),
                        static_cast<int>(c["onvif_port"].i()),
                        std::string(c["username"].s()),
                        crypto::encrypt(std::string(c["password"].s()), key_hex));

                    // RTSP URL'ы — зашифрованы, наружу не отдаются (ТЗ §39)
                    for (const char* key : {"rtsp_url", "rtsp_sub_url"}) {
                        if (c.has(key) && !std::string(c[key].s()).empty()) {
                            const bool is_sub = std::string(key) == "rtsp_sub_url";
                            w.exec_params(
                                "INSERT INTO camera_profiles (camera_id, profile_name, is_substream, rtsp_url_encrypted) "
                                "VALUES (1, $1, $2, $3) "
                                "ON CONFLICT DO NOTHING",
                                is_sub ? "sub" : "main", is_sub,
                                crypto::encrypt(std::string(c[key].s()), key_hex));
                        }
                    }
                }
                w.exec("UPDATE system_settings SET value = 'true', updated_at = now() "
                       "WHERE key = 'setup_completed'");
            });

            spdlog::info("setup completed, admin '{}' created", username);
            cam.reload();
            recorder.reload();

            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });
}

}  // namespace api
