// Camera API (ТЗ §40, §44): настройки камеры, тест соединения.
// GET — любой авторизованный; PATCH/test — только ADMIN (ТЗ §7).
// RTSP credentials НИКОГДА не уходят во frontend (ТЗ §39, §67).
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include "../auth/session.hpp"
#include "../camera/onvif_client.hpp"
#include "../camera/rtsp_probe.hpp"
#include "../common/crypto.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

}  // namespace

void register_camera_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                            CameraManager& cam) {
    // GET /api/camera — настройки без секретов + текущий статус
    CROW_ROUTE(app, "/api/camera")([&db, &cam](const crow::request& req) {
        if (!auth::require_user(req, db)) return json_error(401, "unauthorized");

        crow::json::wvalue res;
        res["status"] = cam.state_str();

        try {
            db.tx([&](pqxx::work& w) {
                const auto r = w.exec(
                    "SELECT id, name, ip_address, onvif_port, username, manufacturer, model, enabled "
                    "FROM camera LIMIT 1");
                if (r.empty()) {
                    res["configured"] = false;
                    return;
                }
                res["configured"] = true;
                res["id"] = r[0]["id"].as<int>();
                res["name"] = r[0]["name"].as<std::string>();
                res["ip_address"] = r[0]["ip_address"].as<std::string>();
                res["onvif_port"] = r[0]["onvif_port"].as<int>();
                res["username"] = r[0]["username"].as<std::string>();
                res["manufacturer"] = r[0]["manufacturer"].as<std::string>();
                res["model"] = r[0]["model"].as<std::string>();
                res["enabled"] = r[0]["enabled"].as<bool>();

                const auto profiles = w.exec(
                    "SELECT profile_name, is_substream, width, height, fps, bitrate, codec "
                    "FROM camera_profiles WHERE camera_id = $1", r[0]["id"].as<int>());
                std::vector<crow::json::wvalue> list;
                for (const auto& p : profiles) {
                    crow::json::wvalue item;
                    item["name"] = p["profile_name"].as<std::string>();
                    item["is_substream"] = p["is_substream"].as<bool>();
                    if (!p["width"].is_null())  item["width"] = p["width"].as<int>();
                    if (!p["height"].is_null()) item["height"] = p["height"].as<int>();
                    if (!p["codec"].is_null())  item["codec"] = p["codec"].as<std::string>();
                    list.push_back(std::move(item));
                }
                res["profiles"] = std::move(list);
            });
        } catch (const std::exception& e) {
            spdlog::error("GET /api/camera failed: {}", e.what());
            return json_error(500, "internal error");
        }
        return crow::response(200, res);
    });

    // PATCH /api/camera — обновление настроек (ADMIN)
    // body: {name?, ip?, onvif_port?, username?, password?, rtsp_url?, rtsp_sub_url?, enabled?}
    CROW_ROUTE(app, "/api/camera").methods(crow::HTTPMethod::PATCH)(
        [&db, &key_hex, &cam](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "admin only");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "invalid json");

            try {
                db.tx([&](pqxx::work& w) {
                    // камера одна — строка с id=1 создаётся при первом сохранении
                    w.exec("INSERT INTO camera (id) VALUES (1) ON CONFLICT (id) DO NOTHING");

                    if (body.has("name"))
                        w.exec_params("UPDATE camera SET name=$1, updated_at=now() WHERE id=1",
                                      std::string(body["name"].s()));
                    if (body.has("ip"))
                        w.exec_params("UPDATE camera SET ip_address=$1, updated_at=now() WHERE id=1",
                                      std::string(body["ip"].s()));
                    if (body.has("onvif_port"))
                        w.exec_params("UPDATE camera SET onvif_port=$1, updated_at=now() WHERE id=1",
                                      static_cast<int>(body["onvif_port"].i()));
                    if (body.has("username"))
                        w.exec_params("UPDATE camera SET username=$1, updated_at=now() WHERE id=1",
                                      std::string(body["username"].s()));
                    if (body.has("password"))
                        w.exec_params("UPDATE camera SET password_encrypted=$1, updated_at=now() WHERE id=1",
                                      crypto::encrypt(std::string(body["password"].s()), key_hex));
                    if (body.has("enabled"))
                        w.exec_params("UPDATE camera SET enabled=$1, updated_at=now() WHERE id=1",
                                      static_cast<bool>(body["enabled"].b()));

                    for (const char* key : {"rtsp_url", "rtsp_sub_url"}) {
                        if (!body.has(key)) continue;
                        const std::string url = body[key].s();
                        const bool is_sub = std::string(key) == "rtsp_sub_url";
                        if (url.empty()) {
                            w.exec_params("DELETE FROM camera_profiles WHERE camera_id=1 AND is_substream=$1",
                                          is_sub);
                        } else {
                            const auto exists = w.exec_params(
                                "SELECT id FROM camera_profiles WHERE camera_id=1 AND is_substream=$1", is_sub);
                            if (exists.empty())
                                w.exec_params(
                                    "INSERT INTO camera_profiles (camera_id, profile_name, is_substream, rtsp_url_encrypted) "
                                    "VALUES (1, $1, $2, $3)",
                                    is_sub ? "sub" : "main", is_sub,
                                    crypto::encrypt(url, key_hex));
                            else
                                w.exec_params(
                                    "UPDATE camera_profiles SET rtsp_url_encrypted=$1, updated_at=now() "
                                    "WHERE camera_id=1 AND is_substream=$2",
                                    crypto::encrypt(url, key_hex), is_sub);
                        }
                    }
                });
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/camera failed: {}", e.what());
                return json_error(500, "internal error");
            }

            cam.reload();
            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // POST /api/camera/test — проверка соединения БЕЗ сохранения (ADMIN)
    // body: {ip, onvif_port, username, password, rtsp_url?}
    CROW_ROUTE(app, "/api/camera/test").methods(crow::HTTPMethod::POST)(
        [&db](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "admin only");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("ip"))
                return json_error(400, "ip required");

            const std::string ip = body["ip"].s();
            const int port = body.has("onvif_port") ? static_cast<int>(body["onvif_port"].i()) : 80;
            const std::string user = body.has("username") ? std::string(body["username"].s()) : "";
            const std::string pass = body.has("password") ? std::string(body["password"].s()) : "";

            crow::json::wvalue res;

            // 1. ONVIF: device info + профили
            OnvifClient onvif(ip, port, user, pass);
            OnvifDeviceInfo info;
            if (onvif.get_device_information(info)) {
                res["onvif"]["ok"] = true;
                res["onvif"]["manufacturer"] = info.manufacturer;
                res["onvif"]["model"] = info.model;
                res["onvif"]["firmware"] = info.firmware;

                std::vector<OnvifProfile> profiles;
                if (onvif.get_profiles(profiles)) {
                    std::vector<crow::json::wvalue> list;
                    for (const auto& p : profiles) {
                        crow::json::wvalue item;
                        item["name"] = p.name;
                        item["token"] = p.token;
                        item["width"] = p.width;
                        item["height"] = p.height;
                        list.push_back(std::move(item));
                    }
                    res["onvif"]["profiles"] = std::move(list);
                }
            } else {
                res["onvif"]["ok"] = false;
                res["onvif"]["error"] = "Камера не отвечает по ONVIF";
            }

            // 2. RTSP: пробуем поток, если URL передан
            if (body.has("rtsp_url") && !std::string(body["rtsp_url"].s()).empty()) {
                res["rtsp"]["ok"] = rtsp::probe(body["rtsp_url"].s());
                if (!static_cast<bool>(res["rtsp"]["ok"]))
                    res["rtsp"]["error"] = "Не удалось подключиться к потоку";
            }

            return crow::response(200, res);
        });
}

}  // namespace api
