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

    StorageManager(db::Database& db, std::string recordings_path, std::string thumbnails_path,
                   std::string hls_path);

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
    // удаляет HLS-сессии архива старше 2 часов (раньше чистились только при
    // создании новой сессии — без новых сессий .ts-чанки лежали бы вечно)
    void cleanup_hls_sessions();
    // удаляет файлы записей без строки в БД (M2 аудита: сбой INSERT после
    // создания файла / краш между open и INSERT → файл невидим для архива
    // и чистки → вечная утечка диска). Запуск — раз в 10 циклов (~10 мин),
    // файлы моложе часа не трогаем (могут быть текущим сегментом)
    void cleanup_orphan_files();

    db::Database& db_;
    std::string recordings_path_;
    std::string thumbnails_path_;
    std::string hls_path_;
    int cycle_count_ = 0;

    EventFn event_fn_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool warned_ = false;
    bool critical_ = false;
};
