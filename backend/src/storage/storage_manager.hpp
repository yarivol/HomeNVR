#pragma once
// Менеджер хранилища (ТЗ §20, §48): circular overwrite.
// При достижении лимита удаляет самые старые сегменты — запись никогда не останавливается.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../db/database.hpp"

class StorageManager {
public:
    using EventFn = std::function<void(const std::string& event, const std::string& payload)>;

    struct Stats {
        std::uint64_t total_bytes = 0;
        std::uint64_t free_bytes = 0;
        std::uint64_t used_bytes = 0;
        std::uint64_t archive_bytes = 0;   // сумма размеров наших сегментов
        double usage_percent = 0;
        std::string oldest_recording;      // ISO-дата или пусто
        std::string newest_recording;
    };

    StorageManager(db::Database& db, std::string recordings_path, std::string thumbnails_path);

    void set_event_callback(EventFn fn) { event_fn_ = std::move(fn); }
    void start();
    void stop();

    Stats stats();

private:
    void run();
    Stats collect();
    double setting_double(const char* key, double fallback);
    void enforce_limit(const Stats& s);
    // удаляет события движения старше самого старого сегмента + их thumbnails
    // (иначе motion_events и JPEG-файлы копятся бесконечно)
    void cleanup_old_events();

    db::Database& db_;
    std::string recordings_path_;
    std::string thumbnails_path_;

    EventFn event_fn_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool warned_ = false;
    bool critical_ = false;
};
