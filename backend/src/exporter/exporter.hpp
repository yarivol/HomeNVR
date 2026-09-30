#pragma once
// Экспорт видео (ТЗ §15-16): склейка сегментов диапазона в один MP4.
// Stream copy — без перекодирования, если поток совместим (ТЗ §15).
// Статусы: QUEUED → PROCESSING → READY/FAILED → EXPIRED (ТЗ §38).
// Готовые файлы живут 1 час, затем удаляются.
#include <atomic>
#include <optional>
#include <string>
#include <thread>

#include "../db/database.hpp"

class Exporter {
public:
    struct ExportInfo {
        long long id;
        std::string status;
        std::string file_name;   // имя файла (без пути)
        std::string start_time;
        std::string end_time;
    };

    Exporter(db::Database& db, std::string recordings_path, std::string exports_path);

    void start();
    void stop();

    // Создаёт задачу экспорта, возвращает id. Бросает std::invalid_argument.
    long long create(const std::string& start_iso, const std::string& end_iso);
    std::optional<ExportInfo> get(long long id);

private:
    void run();
    void process(long long id, const std::string& start_iso, const std::string& end_iso);
    void cleanup_expired();

    db::Database& db_;
    std::string recordings_path_;
    std::string exports_path_;

    std::atomic<bool> running_{false};
    std::thread thread_;
};
