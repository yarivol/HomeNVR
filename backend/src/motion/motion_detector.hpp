#pragma once
// Motion detection (ТЗ §21-26): классический CV-пайплайн на OpenCV.
// Кадры берутся из суб-потока (ТЗ §73.3) или из уменьшенной копии основного.
//
// Pipeline: RTSP frame → resize → grayscale → blur → frame diff → threshold →
//           contours (+ zones) → motion score → событие (ТЗ §21)
//
// Pre-buffer (ТЗ §25): событие начинается за N секунд до детекции.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "../db/database.hpp"

class MotionDetector {
public:
    using EventFn = std::function<void(const std::string& event, const std::string& payload)>;
    // Провайдер URL суб-потока; если суб-потока нет — основной (расшифрованный)
    using UrlProvider = std::function<std::string()>;

    MotionDetector(db::Database& db, std::string thumbnails_path);

    void set_event_callback(EventFn fn) { event_fn_ = std::move(fn); }
    void set_url_provider(UrlProvider p) {
        std::lock_guard lock(mutex_);
        url_provider_ = std::move(p);
    }

    void start();
    void stop();
    void reload();  // настройки motion/zones изменились

    bool enabled();

private:
    void run();
    bool detection_session(const std::string& url);
    std::string current_url();
    void interruptible_sleep(int seconds);

    // Настройки из system_settings
    double sensitivity();        // 0.0..1.0 (слайдер в админке, ТЗ §22)
    int min_event_sec();         // минимальная длительность события (дефолт 3)
    int cooldown_sec();          // пауза между событиями (дефолт 10)

    // Зоны (ТЗ §23): JSON-массивы прямоугольников [{x,y,w,h}] в долях кадра 0..1
    std::string zones_json(const char* key);

    // Создание события + thumbnail (ТЗ §24, §26).
    // started_epoch / span_sec — фактические начало и длительность движения
    // (H1 аудита: раньше писалась жёстко min_event_sec и started_at = конец-3с)
    void save_event(double score, double span_sec, double started_epoch,
                    const std::string& thumbnail_file);

    db::Database& db_;
    std::string thumbnails_path_;

    EventFn event_fn_;
    UrlProvider url_provider_;

    std::atomic<bool> running_{false};
    std::atomic<bool> reload_requested_{false};
    std::thread thread_;
    std::mutex mutex_;
};
