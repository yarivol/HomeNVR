#include "storage_manager.hpp"

#include <spdlog/spdlog.h>
#include <sys/statvfs.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr int kCheckIntervalSec = 60;
constexpr double kWarnThreshold = 0.8;
// Гистерезис очистки: чистим до (порог - 5%), а не до фиксированного
// значения — иначе при пороге < 0.85 очистка вообще не запустится.
constexpr double kCleanupHysteresis = 0.05;
}  // namespace

StorageManager::StorageManager(db::Database& db, std::string recordings_path, std::string thumbnails_path,
                               std::string hls_path)
    : db_(db), recordings_path_(std::move(recordings_path)),
      thumbnails_path_(std::move(thumbnails_path)), hls_path_(std::move(hls_path)) {}

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
// Триггер — ЛЮБОЕ из: usage >= max_storage_usage ИЛИ free < min_free_space (ТЗ §648).
void StorageManager::enforce_limit(const Stats& s) {
    const double max_usage = setting_double("max_storage_usage", 0.9);
    const double min_free = setting_double("min_free_space", 0.1);
    const double threshold = std::min(max_usage, 1.0 - min_free);

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

    if (!overwrite_enabled || s.usage_percent < threshold) {
        // периодический debug-снимок: помогает понять, почему очистка (не) сработала
        spdlog::debug("storage: занято {:.1f}% (архив {:.1f} ГБ), порог {:.0f}%",
                      s.usage_percent * 100, s.archive_bytes / 1073741824.0, threshold * 100);
        return;
    }

    spdlog::warn("storage limit reached ({:.0f}%, threshold {:.0f}%), deleting oldest segments",
                 s.usage_percent * 100, threshold * 100);

    int deleted = 0;
    double usage = s.usage_percent;
    const double cleanup_target = threshold - kCleanupHysteresis;
    while (usage > cleanup_target && deleted < 1000) {
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

// Ретенция событий = ретенция архива: событие без видео бесполезно,
// а его thumbnail на диске — мусор. Удаляем события старше самого старого
// живого сегмента (MIN по записям; если записей нет — NULL и ничего не удалится).
void StorageManager::cleanup_old_events() {
    try {
        const auto thumbs = db_.tx([](pqxx::work& w) {
            const auto r = w.exec(
                "DELETE FROM motion_events "
                "WHERE started_at < (SELECT MIN(started_at) FROM recordings) "
                "RETURNING thumbnail_path");
            std::vector<std::string> out;
            for (const auto& row : r)
                if (!row["thumbnail_path"].is_null())
                    out.push_back(row["thumbnail_path"].as<std::string>());
            return out;
        });
        int removed = 0;
        for (const auto& t : thumbs) {
            // защита от path traversal на всякий случай (значения из БД)
            if (t.find("..") != std::string::npos || t.find('/') != std::string::npos) continue;
            std::error_code ec;
            fs::remove(fs::path(thumbnails_path_) / t, ec);
            if (!ec) ++removed;
        }
        if (!thumbs.empty())
            spdlog::info("events cleanup: {} старых событий удалено ({} thumbnails с диска)",
                         thumbs.size(), removed);
    } catch (const std::exception& e) {
        spdlog::error("events cleanup failed: {}", e.what());
    }
}

void StorageManager::cleanup_hls_sessions() {
    try {
        std::error_code ec;
        if (!fs::exists(hls_path_, ec)) return;
        const auto now = fs::file_time_type::clock::now();
        int removed = 0;
        for (const auto& entry : fs::directory_iterator(hls_path_, ec)) {
            std::error_code ec2;
            if (now - entry.last_write_time(ec2) > std::chrono::hours(2) && !ec2) {
                fs::remove_all(entry.path(), ec2);
                if (!ec2) ++removed;
            }
        }
        if (removed > 0) spdlog::info("hls cleanup: {} устаревших сессий архива удалено", removed);
    } catch (const std::exception& e) {
        spdlog::error("hls cleanup failed: {}", e.what());
    }
}

void StorageManager::cleanup_orphan_files() {
    try {
        const auto known = db_.tx([](pqxx::work& w) {
            std::unordered_set<std::string> paths;
            const auto r = w.exec("SELECT file_path FROM recordings");
            for (const auto& row : r) paths.insert(row["file_path"].as<std::string>());
            return paths;
        });
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(1);
        int removed = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(recordings_path_, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code ec2;
            if (!it->is_regular_file(ec2) || ec2) continue;
            const auto name = it->path().filename().string();
            if (!name.ends_with(".mp4")) continue;
            if (it->last_write_time(ec2) >= cutoff || ec2) continue;  // свежие не трогаем
            const auto rel = fs::relative(it->path(), recordings_path_, ec2).generic_string();
            if (ec2 || known.count(rel)) continue;
            fs::remove(it->path(), ec2);
            if (!ec2) ++removed;
        }
        if (removed > 0)
            spdlog::warn("orphan cleanup: удалено файлов записей без записи в БД: {}", removed);
    } catch (const std::exception& e) {
        spdlog::error("orphan cleanup failed: {}", e.what());
    }
}

void StorageManager::run() {
    while (running_) {
        const Stats s = collect();
        ++cycle_count_;

        // I1: протухшие сессии чистятся не только при новом логине
        try {
            db_.tx([](pqxx::work& w) {
                w.exec("DELETE FROM sessions WHERE expires_at < now()");
            });
        } catch (...) {}

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
        cleanup_old_events();
        cleanup_hls_sessions();
        if (cycle_count_ % 10 == 0) cleanup_orphan_files();

        for (int i = 0; i < kCheckIntervalSec * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
