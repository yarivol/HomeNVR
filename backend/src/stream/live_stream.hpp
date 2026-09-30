#pragma once
// Live HLS (ТЗ §12, §73.1): ffmpeg превращает RTSP в HLS (stream copy).
// ffmpeg-процесс под supervisior'ом: упал → перезапуск (ТЗ §56).
// Задержка ~2–5 сек, работает во всех современных браузерах.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <sys/types.h>

class LiveStream {
public:
    using UrlProvider = std::function<std::string()>;

    explicit LiveStream(std::string live_path);

    void set_url_provider(UrlProvider p) {
        std::lock_guard lock(mutex_);
        url_provider_ = std::move(p);
    }

    void start();
    void stop();
    void reload();

private:
    void run();
    void stop_child();
    std::string current_url();

    std::string live_path_;
    UrlProvider url_provider_;

    std::atomic<bool> running_{false};
    std::atomic<bool> reload_requested_{false};
    pid_t child_ = -1;
    std::thread thread_;
    std::mutex mutex_;
};
