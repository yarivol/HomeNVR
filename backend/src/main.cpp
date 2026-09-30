// HomeNVR backend — точка входа
// Phase 1: инфраструктура (health, status, WS)
// Phase 2: камера (ONVIF, RTSP, reconnect) + auth + setup wizard
#include <crow.h>
#include <spdlog/spdlog.h>

#include "api/routes.hpp"
#include "camera/camera_manager.hpp"
#include "common/config.hpp"
#include "common/crypto.hpp"
#include "db/database.hpp"
#include "ws/ws_hub.hpp"

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::info("HomeNVR backend starting");

    // Конфигурация
    const Config cfg = Config::from_env();
    if (cfg.database_url.empty()) {
        spdlog::error("DATABASE_URL is not set");
        return 1;
    }

    // Ключ шифрования секретов камеры (Docker secret)
    std::string key_hex;
    try {
        key_hex = crypto::load_key_hex(cfg.secret_key_file);
    } catch (const std::exception& e) {
        spdlog::error("secret key: {}", e.what());
        return 1;
    }

    // База данных + миграции
    db::Database db(cfg.database_url);
    try {
        db.connect_with_retry();
        db.run_migrations(cfg.migrations_path);
    } catch (const std::exception& e) {
        spdlog::error("database: {}", e.what());
        return 1;
    }

    // WebSocket-хаб и менеджер камеры
    WsHub ws_hub;
    CameraManager cam;

    cam.set_event_callback([&ws_hub](const std::string& event, const std::string& payload) {
        ws_hub.broadcast(event, payload);
    });

    // Провайдер основного RTSP URL из БД (расшифровка). URL никогда не логируем.
    cam.set_url_provider([&db, &key_hex]() -> std::string {
        return db.tx([&](pqxx::work& w) -> std::string {
            const auto r = w.exec(
                "SELECT p.rtsp_url_encrypted "
                "FROM camera c JOIN camera_profiles p ON p.camera_id = c.id "
                "WHERE c.id = 1 AND c.enabled AND NOT p.is_substream LIMIT 1");
            if (r.empty()) return "";
            return crypto::decrypt(r[0]["rtsp_url_encrypted"].as<std::string>(), key_hex);
        });
    });
    cam.start();

    crow::SimpleApp app;

    // Health check (ТЗ §59) — без авторизации, для Docker
    CROW_ROUTE(app, "/health")([] {
        crow::json::wvalue res;
        res["status"] = "ok";
        return res;
    });

    // Статус системы (ТЗ §50)
    CROW_ROUTE(app, "/api/system/status")([&db, &cam](const crow::request& req) {
        if (!auth::require_user(req, db)) return crow::response(401);
        crow::json::wvalue res;
        res["backend"] = "ok";
        res["database"] = "ok";
        res["camera"] = cam.state_str();
        res["recording"] = "stopped";   // Phase 3
        res["motion"] = "disabled";     // Phase 4
        res["storage_percent"] = 0;     // Phase 3
        return res;
    });

    // WebSocket (ТЗ §41)
    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&ws_hub](crow::websocket::connection& conn) {
            spdlog::info("ws client connected");
            ws_hub.add(&conn);
        })
        .onclose([&ws_hub](crow::websocket::connection& conn, const std::string&, uint16_t) {
            ws_hub.remove(&conn);
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) {});

    // API
    api::register_auth_routes(app, db);
    api::register_setup_routes(app, db, key_hex, cam);
    api::register_camera_routes(app, db, key_hex, cam);

    spdlog::info("listening on port {}", cfg.port);
    app.port(cfg.port).multithreaded().run();

    cam.stop();
    return 0;
}
