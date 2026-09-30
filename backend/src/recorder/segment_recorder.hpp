#pragma once
// Сегментный рекордер (ТЗ §17-19): RTSP -> MP4 сегменты по 5 минут (stream copy).
// Ротация на кейфрейме — каждый сегмент самодостаточен и воспроизводим.
// Metadata каждого сегмента пишется в таблицу recordings (ТЗ §36).
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "../db/database.hpp"

class SegmentRecorder {
public:
    using EventFn = std::function<void(const std::string& event, const std::string& payload)>;
    using UrlProvider = std::function<std::string()>;  // основной RTSP URL (расшифрованный)

    SegmentRecorder(db::Database& db, std::string recordings_path);

    void set_event_callback(EventFn fn) { event_fn_ = std::move(fn); }
    void set_url_provider(UrlProvider p) {
        std::lock_guard lock(mutex_);
        url_provider_ = std::move(p);
    }

    void start();
    void stop();
    void reload();  // настройки изменились — перезапустить запись

    // Уведомление от motion-детектора ("motion.started" / "motion.ended") —
    // используется в режиме записи "по движению" (ТЗ §18)
    void on_motion_event(const std::string& event);

    // "running" | "stopped" | "error" (ТЗ §50)
    std::string state_str() const;

private:
    void run();
    // Одна сессия записи: от открытия потока до обрыва. false = ошибка.
    bool record_session(const std::string& url, int segment_sec);
    // Сессия в режиме "по движению": буферизация pre-buffer, запись пока есть движение
    bool record_session_motion(const std::string& url, int segment_sec, int pre_buffer_sec);
    std::string current_url();
    int segment_duration();
    int pre_buffer_sec();
    int motion_cooldown_sec();
    std::string recording_mode();
    void interruptible_sleep(int seconds);

    db::Database& db_;
    std::string recordings_path_;

    EventFn event_fn_;
    UrlProvider url_provider_;

    std::atomic<int> state_{0};  // 0=stopped 1=running 2=error
    std::atomic<bool> running_{false};
    std::atomic<bool> reload_requested_{false};
    std::atomic<bool> motion_active_{false};
    std::atomic<std::chrono::steady_clock::time_point> last_motion_end_{
        std::chrono::steady_clock::time_point::min()};
    std::thread thread_;
    std::mutex mutex_;
};
