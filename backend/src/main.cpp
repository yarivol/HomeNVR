// HomeNVR backend — точка входа
// Phase 1: инфраструктура (health, status, WS)
// Phase 2: камера (ONVIF, RTSP, reconnect) + auth + setup wizard
// Phase 3: запись (сегменты, stream copy) + хранилище (circular overwrite)
#include <crow.h>
#include <spdlog/spdlog.h>

#include "api/routes.hpp"
#include "camera/camera_manager.hpp"
#include "common/config.hpp"
#include "common/crypto.hpp"
#include "db/database.hpp"
#include "recorder/segment_recorder.hpp"
#include "motion/motion_detector.hpp"
#include "storage/storage_manager.hpp"
#include "stream/live_stream.hpp"
#include "exporter/exporter.hpp"
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
    auto main_rtsp_url = [&db, &key_hex]() -> std::string {
        return db.tx([&](pqxx::work& w) -> std::string {
            const auto r = w.exec(
                "SELECT p.rtsp_url_encrypted "
                "FROM camera c JOIN camera_profiles p ON p.camera_id = c.id "
                "WHERE c.id = 1 AND c.enabled AND NOT p.is_substream LIMIT 1");
            if (r.empty()) return "";
            return crypto::decrypt(r[0]["rtsp_url_encrypted"].as<std::string>(), key_hex);
        });
    };
    cam.set_url_provider(main_rtsp_url);
    cam.start();

    // Рекордер (Phase 3): пишет сегменты по 5 минут, stream copy
    SegmentRecorder recorder(db, cfg.recordings_path);
    recorder.set_event_callback([&ws_hub](const std::string& event, const std::string& payload) {
        ws_hub.broadcast(event, payload);
    });
    recorder.set_url_provider(main_rtsp_url);
    recorder.start();

    // Хранилище (Phase 3): circular overwrite, предупреждения о заполнении
    StorageManager storage(db, cfg.recordings_path);
    storage.set_event_callback([&ws_hub](const std::string& event, const std::string& payload) {
        ws_hub.broadcast(event, payload);
    });
    storage.start();

    // Motion detection (Phase 4): предпочитаем суб-поток, fallback — основной (ТЗ §73.3)
    MotionDetector motion(db, cfg.thumbnails_path);
    motion.set_event_callback([&ws_hub](const std::string& event, const std::string& payload) {
        ws_hub.broadcast(event, payload);
    });
    motion.set_url_provider([&db, &key_hex]() -> std::string {
        return db.tx([&](pqxx::work& w) -> std::string {
            const auto r = w.exec(
                "SELECT p.rtsp_url_encrypted FROM camera c "
                "JOIN camera_profiles p ON p.camera_id = c.id "
                "WHERE c.id = 1 AND c.enabled "
                "ORDER BY p.is_substream DESC LIMIT 1");  // сначала суб-поток
            if (r.empty()) return "";
            return crypto::decrypt(r[0]["rtsp_url_encrypted"].as<std::string>(), key_hex);
        });
    });
    motion.start();

    // Live HLS (Phase 5): ffmpeg RTSP→HLS под супервизором
    LiveStream live(cfg.live_path);
    live.set_url_provider(main_rtsp_url);
    live.start();

    // Экспорт MP4 (Phase 5): очередь задач, stream copy
    Exporter exporter(db, cfg.recordings_path, cfg.exports_path);
    exporter.start();

    crow::SimpleApp app;

    // Health check (ТЗ §59) — без авторизации, для Docker
    CROW_ROUTE(app, "/health")([] {
        crow::json::wvalue res;
        res["status"] = "ok";
        return res;
    });

    // Статус системы (ТЗ §50)
    CROW_ROUTE(app, "/api/system/status")([&db, &cam, &recorder, &storage, &motion](const crow::request& req) {
        if (!auth::require_user(req, db)) return crow::response(401);
        crow::json::wvalue res;
        res["backend"] = "ok";
        res["database"] = "ok";
        res["camera"] = cam.state_str();
        res["recording"] = recorder.state_str();
        res["motion"] = motion.enabled() ? "enabled" : "disabled";
        res["storage_percent"] = static_cast<int>(storage.stats().usage_percent * 100);
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
    api::register_setup_routes(app, db, key_hex, cam, recorder);
    api::register_camera_routes(app, db, key_hex, cam, recorder, live);
    api::register_storage_routes(app, db, storage);
    api::register_events_routes(app, db, cfg.thumbnails_path);
    api::register_stream_routes(app, db, exporter, cfg.live_path, cfg.hls_path,
                                cfg.recordings_path);
    api::register_admin_routes(app, db, motion, recorder);

    spdlog::info("listening on port {}", cfg.port);
    app.port(cfg.port).multithreaded().run();

    exporter.stop();
    live.stop();
    motion.stop();
    recorder.stop();
    storage.stop();
    cam.stop();
    return 0;
}
