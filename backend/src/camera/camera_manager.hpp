#pragma once
// Менеджер камеры: state machine CONNECTED → DISCONNECTED → RECONNECTING (ТЗ §57).
// Работает в отдельном потоке, периодически проверяет RTSP-поток.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

enum class CameraState { Disconnected, Reconnecting, Connected };

class CameraManager {
public:
    // Событие для WS-рассылки: имя + JSON payload
    using EventFn = std::function<void(const std::string& event, const std::string& payload)>;
    // Провайдер актуального RTSP URL (читает из БД, расшифровывает)
    using UrlProvider = std::function<std::string()>;

    void set_event_callback(EventFn fn) { event_fn_ = std::move(fn); }
    void set_url_provider(UrlProvider p) {
        std::lock_guard lock(mutex_);
        url_provider_ = std::move(p);
    }

    void start();
    void stop();
    void reload();  // настройки изменились — переподключиться

    CameraState state() const { return state_; }
    std::string state_str() const;

private:
    void run();
    void set_state(CameraState s);
    std::string current_url();
    void interruptible_sleep(int seconds);

    EventFn event_fn_;
    UrlProvider url_provider_;

    std::atomic<CameraState> state_{CameraState::Disconnected};
    std::atomic<bool> running_{false};
    std::atomic<bool> reload_requested_{false};
    std::thread thread_;
    std::mutex mutex_;
};
