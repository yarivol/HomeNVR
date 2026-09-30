#pragma once
// Регистрация всех API-роутов (ТЗ §40).
// Каждый защищённый endpoint проверяет сессию и роль (ТЗ §67).
#include <crow.h>

#include "../camera/camera_manager.hpp"
#include "../db/database.hpp"

class StorageManager;
class SegmentRecorder;
class LiveStream;
class Exporter;
class MotionDetector;

namespace api {

void register_auth_routes(crow::SimpleApp& app, db::Database& db);
void register_setup_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                           CameraManager& cam, SegmentRecorder& recorder);
void register_camera_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                            CameraManager& cam, SegmentRecorder& recorder, LiveStream& live);
void register_storage_routes(crow::SimpleApp& app, db::Database& db, StorageManager& storage);
void register_events_routes(crow::SimpleApp& app, db::Database& db,
                            const std::string& thumbnails_path);
void register_stream_routes(crow::SimpleApp& app, db::Database& db, Exporter& exporter,
                            const std::string& live_path, const std::string& hls_path,
                            const std::string& recordings_path);
void register_admin_routes(crow::SimpleApp& app, db::Database& db, MotionDetector& motion,
                           SegmentRecorder& recorder, const std::string& logs_path);

}  // namespace api
