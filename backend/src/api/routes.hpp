#pragma once
// Регистрация всех API-роутов (ТЗ §40).
// Каждый защищённый endpoint проверяет сессию и роль (ТЗ §67).
#include <crow.h>

#include "../camera/camera_manager.hpp"
#include "../db/database.hpp"

class StorageManager;
class SegmentRecorder;

namespace api {

void register_auth_routes(crow::SimpleApp& app, db::Database& db);
void register_setup_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                           CameraManager& cam, SegmentRecorder& recorder);
void register_camera_routes(crow::SimpleApp& app, db::Database& db, const std::string& key_hex,
                            CameraManager& cam, SegmentRecorder& recorder);
void register_storage_routes(crow::SimpleApp& app, db::Database& db, StorageManager& storage);

}  // namespace api
