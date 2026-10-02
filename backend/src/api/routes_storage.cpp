// Storage API (ТЗ §40, §48): статистика хранилища.
#include "routes.hpp"

#include "../auth/session.hpp"
#include "../storage/storage_manager.hpp"

namespace api {

void register_storage_routes(crow::SimpleApp& app, db::Database& db, StorageManager& storage) {
    CROW_ROUTE(app, "/api/storage")([&db, &storage](const crow::request& req) {
        if (!auth::require_user(req, db)) return crow::response(401);

        const auto s = storage.stats();
        crow::json::wvalue res;
        res["total_bytes"] = s.total_bytes;
        res["free_bytes"] = s.free_bytes;
        res["used_bytes"] = s.used_bytes;
        res["archive_bytes"] = s.archive_bytes;
        // M5 аудита: сколько занято НЕ нашим архивом (БД, логи, чужие данные) —
        // если это заполнило диск, cleanup стирает записи «впустую»
        res["non_archive_bytes"] = s.used_bytes > s.archive_bytes
                                       ? s.used_bytes - s.archive_bytes
                                       : std::uint64_t(0);
        res["usage_percent"] = static_cast<int>(s.usage_percent * 100);
        if (!s.oldest_recording.empty()) res["oldest_recording"] = s.oldest_recording;
        if (!s.newest_recording.empty()) res["newest_recording"] = s.newest_recording;
        return crow::response(200, res);
    });
}

}  // namespace api
