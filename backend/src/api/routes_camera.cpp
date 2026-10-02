// Camera API (ТЗ §40, §44): настройки камеры, тест соединения.
// GET — любой авторизованный; PATCH/test — только ADMIN (ТЗ §7).
// RTSP credentials НИКОГДА не уходят во frontend (ТЗ §39, §67).
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include <optional>

#include "../auth/session.hpp"
#include "../camera/onvif_client.hpp"
#include "../camera/rtsp_probe.hpp"
#include "../common/crypto.hpp"
#include "../recorder/segment_recorder.hpp"
#include "../stream/live_stream.hpp"

namespace api {
namespace {

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

// L6: RTSP URL уходит в ffmpeg/libav — только rtsp:// и rtsps://
bool valid_rtsp_url(const std::string& u) {
    return u.rfind("rtsp://", 0) == 0 || u.rfind("rtsps://", 0) == 0;
}

}  // namespace

void register_camera_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                            CameraManager& cam, SegmentRecorder& recorder, LiveStream& live) {
    // GET /api/camera — настройки без секретов + текущий статус
    CROW_ROUTE(app, "/api/camera")([&db, &cam](const crow::request& req) {
        if (!auth::require_user(req, db)) return json_error(401, "Нет авторизации");

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
            return json_error(500, "Внутренняя ошибка сервера");
        }
        return crow::response(200, res);
    });

    // PATCH /api/camera — обновление настроек (ADMIN)
    // body: {name?, ip?, onvif_port?, username?, password?, rtsp_url?, rtsp_sub_url?, enabled?}
    CROW_ROUTE(app, "/api/camera").methods(crow::HTTPMethod::PATCH)(
        [&db, &key_hex, &cam, &recorder, &live](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "Некорректный JSON");

            // H5: разбор и валидация ДО транзакции — исключение из .s()/.i()/.b()
            // не покидает handler; ошибочные типы дают 400, а не 500/crash
            std::optional<std::string> v_name, v_ip, v_user, v_pass, v_rtsp, v_rtsp_sub;
            std::optional<int> v_port;
            std::optional<bool> v_enabled;
            try {
                if (body.has("name"))     v_name = std::string(body["name"].s());
                if (body.has("ip"))       v_ip = std::string(body["ip"].s());
                if (body.has("onvif_port")) v_port = static_cast<int>(body["onvif_port"].i());
                if (body.has("username")) v_user = std::string(body["username"].s());
                if (body.has("password")) v_pass = std::string(body["password"].s());
                if (body.has("enabled"))  v_enabled = body["enabled"].b();
                if (body.has("rtsp_url")) v_rtsp = std::string(body["rtsp_url"].s());
                if (body.has("rtsp_sub_url")) v_rtsp_sub = std::string(body["rtsp_sub_url"].s());
            } catch (const std::exception&) {
                return json_error(400, "Неверный тип значения");
            }
            if (v_port.has_value() && (*v_port < 1 || *v_port > 65535))
                return json_error(400, "Порт должен быть 1–65535");
            if (v_rtsp.has_value() && !v_rtsp->empty() && !valid_rtsp_url(*v_rtsp))
                return json_error(400, "RTSP URL должен начинаться с rtsp://");
            if (v_rtsp_sub.has_value() && !v_rtsp_sub->empty() && !valid_rtsp_url(*v_rtsp_sub))
                return json_error(400, "RTSP URL должен начинаться с rtsp://");

            try {
                db.tx([&](pqxx::work& w) {
                    // камера одна — строка с id=1 создаётся при первом сохранении
                    w.exec("INSERT INTO camera (id) VALUES (1) ON CONFLICT (id) DO NOTHING");

                    if (v_name.has_value())
                        w.exec_params("UPDATE camera SET name=$1, updated_at=now() WHERE id=1", *v_name);
                    if (v_ip.has_value())
                        w.exec_params("UPDATE camera SET ip_address=$1, updated_at=now() WHERE id=1", *v_ip);
                    if (v_port.has_value())
                        w.exec_params("UPDATE camera SET onvif_port=$1, updated_at=now() WHERE id=1", *v_port);
                    if (v_user.has_value())
                        w.exec_params("UPDATE camera SET username=$1, updated_at=now() WHERE id=1", *v_user);
                    if (v_pass.has_value())
                        w.exec_params("UPDATE camera SET password_encrypted=$1, updated_at=now() WHERE id=1",
                                      crypto::encrypt(*v_pass, key_hex));
                    if (v_enabled.has_value())
                        w.exec_params("UPDATE camera SET enabled=$1, updated_at=now() WHERE id=1", *v_enabled);

                    struct UrlUpdate { const std::optional<std::string>* val; bool is_sub; };
                    for (const auto& u : {UrlUpdate{&v_rtsp, false}, UrlUpdate{&v_rtsp_sub, true}}) {
                        if (!u.val->has_value()) continue;
                        const std::string& url = **u.val;
                        if (url.empty()) {
                            w.exec_params("DELETE FROM camera_profiles WHERE camera_id=1 AND is_substream=$1",
                                          u.is_sub);
                        } else {
                            const auto exists = w.exec_params(
                                "SELECT id FROM camera_profiles WHERE camera_id=1 AND is_substream=$1", u.is_sub);
                            if (exists.empty())
                                w.exec_params(
                                    "INSERT INTO camera_profiles (camera_id, profile_name, is_substream, rtsp_url_encrypted) "
                                    "VALUES (1, $1, $2, $3)",
                                    u.is_sub ? "sub" : "main", u.is_sub,
                                    crypto::encrypt(url, key_hex));
                            else
                                w.exec_params(
                                    "UPDATE camera_profiles SET rtsp_url_encrypted=$1, updated_at=now() "
                                    "WHERE camera_id=1 AND is_substream=$2",
                                    crypto::encrypt(url, key_hex), u.is_sub);
                        }
                    }
                });
            } catch (const std::exception& e) {
                spdlog::error("PATCH /api/camera failed: {}", e.what());
                return json_error(500, "Внутренняя ошибка сервера");
            }

            // аудит изменений (без секретов: пароли/URL не логируем)
            std::string changed;
            for (const char* k : {"name", "ip", "onvif_port", "username", "password",
                                  "enabled", "rtsp_url", "rtsp_sub_url"})
                if (body.has(k)) {
                    if (!changed.empty()) changed += ", ";
                    changed += k;
                    if (std::string(k) == "enabled")
                        changed += std::string("=") + (body["enabled"].b() ? "true" : "false");
                }
            spdlog::info("admin: настройки камеры изменены: {}", changed);

            cam.reload();
            recorder.reload();
            live.reload();
            crow::json::wvalue res;
            res["ok"] = true;
            return crow::response(200, res);
        });

    // POST /api/camera/test — проверка соединения БЕЗ сохранения (ADMIN)
    // body: {ip, onvif_port, username, password, rtsp_url?}
    CROW_ROUTE(app, "/api/camera/test").methods(crow::HTTPMethod::POST)(
        [&db](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body || !body.has("ip"))
                return json_error(400, "ip required");

            std::string ip, user, pass, rtsp_url;
            int port = 80;
            try {
                ip = body["ip"].s();
                if (body.has("onvif_port")) port = static_cast<int>(body["onvif_port"].i());
                if (body.has("username")) user = body["username"].s();
                if (body.has("password")) pass = body["password"].s();
                if (body.has("rtsp_url")) rtsp_url = body["rtsp_url"].s();
            } catch (const std::exception&) {
                return json_error(400, "Неверный тип значения");
            }

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
            if (!rtsp_url.empty()) {
                if (!valid_rtsp_url(rtsp_url))
                    return json_error(400, "RTSP URL должен начинаться с rtsp://");
                const bool rtsp_ok = rtsp::probe(rtsp_url);
                res["rtsp"]["ok"] = rtsp_ok;
                if (!rtsp_ok)
                    res["rtsp"]["error"] = "Не удалось подключиться к потоку";
            }

            return crow::response(200, res);
        });

    // ---- Видео-настройки камеры через ONVIF (ТЗ §29, §45) ----

    // реквизиты камеры из БД (пароль расшифровывается только здесь, во frontend не уходит)
    // nullopt = камера вообще не настроена (нет строки). Пустой ip — не ошибка:
    // ONVIF просто недоступен, GET отвечает 200 {onvif:false} (graceful degradation).
    auto load_creds = [&db]() -> std::optional<std::tuple<std::string, int, std::string, std::string>> {
        try {
            return db.tx([](pqxx::work& w) -> std::optional<std::tuple<std::string, int, std::string, std::string>> {
                const auto r = w.exec(
                    "SELECT ip_address, onvif_port, username, password_encrypted FROM camera LIMIT 1");
                if (r.empty()) return std::nullopt;
                return std::make_tuple(r[0]["ip_address"].as<std::string>(),
                                       r[0]["onvif_port"].as<int>(),
                                       r[0]["username"].as<std::string>(),
                                       r[0]["password_encrypted"].as<std::string>());
            });
        } catch (...) {
            return std::nullopt;
        }
    };

    // GET /api/camera/video — текущие параметры + доступные опции (ADMIN)
    CROW_ROUTE(app, "/api/camera/video")([&db, &key_hex, load_creds](const crow::request& req) {
        if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

        crow::json::wvalue res;
        const auto creds = load_creds();
        if (!creds) return json_error(400, "Камера не настроена");

        const auto& [ip, port, user, pass_enc] = *creds;
        if (ip.empty()) {
            res["onvif"] = false;
            res["error"] = "ONVIF недоступен — не задан IP-адрес камеры";
            return crow::response(200, res);
        }
        const std::string pass = pass_enc.empty() ? "" : crypto::decrypt(pass_enc, key_hex);
        OnvifClient onvif(ip, port, user, pass);

        std::vector<OnvifProfile> profiles;
        VideoEncoderConfig cfg;
        if (!onvif.get_profiles(profiles)) {
            res["onvif"] = false;
            res["error"] = "Камера не отвечает по ONVIF — удалённая настройка видео недоступна";
            return crow::response(200, res);
        }
        if (!onvif.get_video_encoder_config(profiles.front().token, cfg)) {
            res["onvif"] = false;
            res["error"] = "Камера не отдаёт конфигурацию видеокодировщика";
            return crow::response(200, res);
        }

        res["onvif"] = true;
        res["profile"] = profiles.front().name;
        res["config"]["encoding"] = cfg.encoding;
        res["config"]["width"] = cfg.width;
        res["config"]["height"] = cfg.height;
        res["config"]["fps"] = cfg.fps;
        res["config"]["bitrate_kbps"] = cfg.bitrate_kbps;
        res["config"]["quality"] = cfg.quality;

        VideoEncoderOptions opts;
        if (onvif.get_video_encoder_options(profiles.front().token, cfg.token, opts)) {
            std::vector<crow::json::wvalue> res_list;
            for (const auto& [w, h] : opts.resolutions) {
                crow::json::wvalue item;
                item["width"] = w;
                item["height"] = h;
                res_list.push_back(std::move(item));
            }
            res["options"]["resolutions"] = std::move(res_list);
            res["options"]["fps_min"] = opts.fps_min;
            res["options"]["fps_max"] = opts.fps_max;
            res["options"]["bitrate_min"] = opts.bitrate_min;
            res["options"]["bitrate_max"] = opts.bitrate_max;
        }
        return crow::response(200, res);
    });

    // PATCH /api/camera/video {width, height, fps, bitrate_kbps} — изменение (ADMIN)
    // ТЗ §29: успех показываем только если камера ФАКТИЧЕСКИ приняла значения.
    CROW_ROUTE(app, "/api/camera/video").methods(crow::HTTPMethod::PATCH)(
        [&db, &key_hex, load_creds](const crow::request& req) {
            if (!auth::require_admin(req, db)) return json_error(403, "Требуются права администратора");

            const auto body = crow::json::load(req.body);
            if (!body) return json_error(400, "Некорректный JSON");

            // H5: типы проверяются до использования
            std::optional<int> v_w, v_h, v_fps, v_br;
            try {
                if (body.has("width"))        v_w = static_cast<int>(body["width"].i());
                if (body.has("height"))       v_h = static_cast<int>(body["height"].i());
                if (body.has("fps"))          v_fps = static_cast<int>(body["fps"].i());
                if (body.has("bitrate_kbps")) v_br = static_cast<int>(body["bitrate_kbps"].i());
            } catch (const std::exception&) {
                return json_error(400, "Неверный тип значения (width/height/fps/bitrate_kbps)");
            }

            const auto creds = load_creds();
            if (!creds) return json_error(400, "Камера не настроена");

            const auto& [ip, port, user, pass_enc] = *creds;
            if (ip.empty())
                return json_error(400, "ONVIF недоступен — не задан IP-адрес камеры");
            const std::string pass = pass_enc.empty() ? "" : crypto::decrypt(pass_enc, key_hex);
            OnvifClient onvif(ip, port, user, pass);

            std::vector<OnvifProfile> profiles;
            if (!onvif.get_profiles(profiles))
                return json_error(400, "Камера не отвечает по ONVIF");

            // полная текущая конфигурация — Set требует вернуть её целиком
            VideoEncoderConfig cfg;
            if (!onvif.get_video_encoder_config(profiles.front().token, cfg))
                return json_error(400, "Камера не отдаёт конфигурацию видеокодировщика");

            const int new_w = v_w.value_or(cfg.width);
            const int new_h = v_h.value_or(cfg.height);
            const int new_fps = v_fps.value_or(cfg.fps);
            const int new_br = v_br.value_or(cfg.bitrate_kbps);
            // базовая sanity-валидация даже без опций камеры
            if (new_w < 0 || new_h < 0 || new_fps < 0 || new_br < 0)
                return json_error(400, "Значения не могут быть отрицательными");

            // валидация по опциям камеры, если она их отдаёт
            VideoEncoderOptions opts;
            if (onvif.get_video_encoder_options(profiles.front().token, cfg.token, opts)) {
                if (!opts.resolutions.empty()) {
                    bool ok = false;
                    for (const auto& [w, h] : opts.resolutions)
                        if (w == new_w && h == new_h) { ok = true; break; }
                    if (!ok) return json_error(400, "Разрешение не поддерживается камерой");
                }
                if (opts.fps_max > 0 && (new_fps < opts.fps_min || new_fps > opts.fps_max))
                    return json_error(400, "FPS вне диапазона камеры");
                if (opts.bitrate_max > 0 && (new_br < opts.bitrate_min || new_br > opts.bitrate_max))
                    return json_error(400, "Битрейт вне диапазона камеры");
            }

            cfg.width = new_w; cfg.height = new_h; cfg.fps = new_fps; cfg.bitrate_kbps = new_br;
            if (!onvif.set_video_encoder_config(cfg))
                return json_error(400, "Не поддерживается камерой");

            // перечитываем и проверяем, что камера применила значения
            VideoEncoderConfig verify;
            if (onvif.get_video_encoder_config(profiles.front().token, verify) &&
                (verify.width != new_w || verify.height != new_h ||
                 (new_fps > 0 && verify.fps != new_fps) ||
                 (new_br > 0 && verify.bitrate_kbps != new_br))) {
                spdlog::warn("camera accepted Set but values differ: {}x{} fps={} br={}",
                             verify.width, verify.height, verify.fps, verify.bitrate_kbps);
                return json_error(400, "Камера не применила значения");
            }

            spdlog::info("admin: видео-параметры камеры изменены: {}x{} fps={} bitrate={}",
                         new_w, new_h, new_fps, new_br);
            crow::json::wvalue res;
            res["ok"] = true;
            res["config"]["width"] = new_w;
            res["config"]["height"] = new_h;
            res["config"]["fps"] = new_fps;
            res["config"]["bitrate_kbps"] = new_br;
            return crow::response(200, res);
        });
}

}  // namespace api
