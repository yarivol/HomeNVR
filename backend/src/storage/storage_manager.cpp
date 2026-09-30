#include "storage_manager.hpp"

#include <spdlog/spdlog.h>
#include <sys/statvfs.h>

#include <chrono>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace {
constexpr int kCheckIntervalSec = 60;
constexpr double kWarnThreshold = 0.8;
constexpr double kCleanupTarget = 0.85;  // чистим с запасом до 85%
}  // namespace

StorageManager::StorageManager(db::Database& db, std::string recordings_path)
    : db_(db), recordings_path_(std::move(recordings_path)) {}

void StorageManager::start() {
    if (running_.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(recordings_path_, ec);
    thread_ = std::thread([this] { run(); });
    spdlog::info("storage manager started");
}

void StorageManager::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

double StorageManager::setting_double(const char* key, double fallback) {
    try {
        return db_.tx([&](pqxx::work& w) {
            return std::stod(
                w.exec_params("SELECT value::text FROM system_settings WHERE key=$1", key)[0][0]
                    .as<std::string>());
        });
    } catch (...) {
        return fallback;
    }
}

StorageManager::Stats StorageManager::collect() {
    Stats s;
    struct statvfs vfs {};
    if (statvfs(recordings_path_.c_str(), &vfs) != 0) return s;

    s.total_bytes = static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    s.free_bytes  = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    s.used_bytes  = s.total_bytes - s.free_bytes;
    s.usage_percent = s.total_bytes ? static_cast<double>(s.used_bytes) / s.total_bytes : 0;

    try {
        db_.tx([&](pqxx::work& w) {
            const auto r = w.exec(
                "SELECT COALESCE(SUM(file_size),0) AS archive, "
                "       MIN(started_at)::text AS oldest, MAX(started_at)::text AS newest "
                "FROM recordings");
            s.archive_bytes = r[0]["archive"].as<long long>();
            if (!r[0]["oldest"].is_null()) s.oldest_recording = r[0]["oldest"].as<std::string>();
            if (!r[0]["newest"].is_null()) s.newest_recording = r[0]["newest"].as<std::string>();
        });
    } catch (const std::exception& e) {
        spdlog::error("storage stats failed: {}", e.what());
    }
    return s;
}

StorageManager::Stats StorageManager::stats() {
    return collect();
}

// Circular overwrite (ТЗ §20): удаляем старые сегменты, пока не освободим место.
// Приоритет — непрерывность записи.
void StorageManager::enforce_limit(const Stats& s) {
    const double max_usage = setting_double("max_storage_usage", 0.9);

    const auto overwrite_enabled = [&] {
        try {
            return db_.tx([](pqxx::work& w) {
                return w.exec("SELECT value::text FROM system_settings WHERE key='overwrite_enabled'")[0][0]
                           .as<std::string>() == "true";
            });
        } catch (...) {
            return true;
        }
    }();

    if (!overwrite_enabled || s.usage_percent < max_usage) return;

    spdlog::warn("storage limit reached ({:.0f}%), deleting oldest segments", s.usage_percent * 100);

    int deleted = 0;
    double usage = s.usage_percent;
    while (usage > kCleanupTarget && deleted < 1000) {
        // самый старый закрытый сегмент (текущий не трогаем: ended_at IS NOT NULL)
        auto oldest = db_.tx([](pqxx::work& w) -> std::optional<std::pair<long long, std::string>> {
            const auto r = w.exec(
                "SELECT id, file_path FROM recordings "
                "WHERE ended_at IS NOT NULL ORDER BY started_at ASC LIMIT 1");
            if (r.empty()) return std::nullopt;
            return std::make_pair(r[0]["id"].as<long long>(),
                                  r[0]["file_path"].as<std::string>());
        });
        if (!oldest) break;  // больше нечего удалять

        std::error_code ec;
        fs::remove(fs::path(recordings_path_) / oldest->second, ec);
        if (ec) spdlog::warn("cannot delete file (already gone?): {}", oldest->second);

        db_.tx([&](pqxx::work& w) {
            // события движения, ссылающиеся на сегмент, отвязываются (ON DELETE SET NULL)
            w.exec_params("DELETE FROM recordings WHERE id=$1", oldest->first);
        });
        ++deleted;
        usage = collect().usage_percent;
    }

    if (deleted > 0)
        spdlog::info("storage cleanup: {} segments deleted, usage now {:.0f}%",
                     deleted, usage * 100);
}

void StorageManager::run() {
    while (running_) {
        const Stats s = collect();

        // уведомления storage.warning / storage.critical (ТЗ §41) — один раз на переход
        const double max_usage = setting_double("max_storage_usage", 0.9);
        const std::string payload =
            "{\"percent\":" + std::to_string(static_cast<int>(s.usage_percent * 100)) + "}";

        if (s.usage_percent >= max_usage && !critical_) {
            critical_ = true;
            if (event_fn_) event_fn_("storage.critical", payload);
        } else if (s.usage_percent >= kWarnThreshold && !warned_) {
            warned_ = true;
            if (event_fn_) event_fn_("storage.warning", payload);
        } else if (s.usage_percent < kWarnThreshold) {
            warned_ = critical_ = false;
        }

        enforce_limit(s);

        for (int i = 0; i < kCheckIntervalSec * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
