#include "live_stream.hpp"

#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

LiveStream::LiveStream(std::string live_path) : live_path_(std::move(live_path)) {}

void LiveStream::start() {
    if (running_.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(live_path_, ec);
    thread_ = std::thread([this] { run(); });
    spdlog::info("live stream supervisor started");
}

void LiveStream::stop() {
    running_ = false;
    stop_child();
    if (thread_.joinable()) thread_.join();
}

void LiveStream::reload() { reload_requested_ = true; }

std::string LiveStream::current_url() {
    std::lock_guard lock(mutex_);
    if (!url_provider_) return "";
    try {
        return url_provider_();
    } catch (...) {
        return "";
    }
}

void LiveStream::stop_child() {
    if (child_ > 0) {
        kill(child_, SIGTERM);
        for (int i = 0; i < 20; ++i) {  // до 2 секунд на graceful stop
            int status = 0;
            if (waitpid(child_, &status, WNOHANG) > 0) break;
            usleep(100'000);
        }
        kill(child_, SIGKILL);
        waitpid(child_, nullptr, 0);
        child_ = -1;
    }
}

void LiveStream::run() {
    while (running_) {
        const std::string url = current_url();
        if (url.empty()) {
            for (int i = 0; i < 50 && running_ && !reload_requested_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // ffmpeg: RTSP -> HLS (stream copy, минимальная нагрузка).
        // -timeout: замерший RTSP-обрыв не должен оставлять «живой» процесс
        // с мёртвым плейлистом (супервизор ждёт только exit).
        // rw_timeout не ставим: как опция ввода ffmpeg CLI его не принимает
        const std::string playlist = live_path_ + "/index.m3u8";
        const std::string rtsp_timeout = "15000000";  // 15 c, мкс
        child_ = fork();
        if (child_ == 0) {
            execlp("ffmpeg", "ffmpeg",
                   "-loglevel", "warning",
                   "-rtsp_transport", "tcp",
                   "-timeout", rtsp_timeout.c_str(),
                   "-i", url.c_str(),
                   "-c", "copy",
                   "-f", "hls",
                   "-hls_time", "2",
                   "-hls_list_size", "6",
                   "-hls_flags", "delete_segments+program_date_time",
                   playlist.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);  // execlp не удался
        }

        if (child_ < 0) {
            spdlog::error("live: fork failed");
            std::this_thread::sleep_for(std::chrono::seconds(10));
            continue;
        }
        spdlog::info("live hls started (pid {})", static_cast<int>(child_));

        // ждём завершения ffmpeg или команды stop/reload
        int status = 0;
        while (running_ && !reload_requested_) {
            if (waitpid(child_, &status, WNOHANG) > 0) {
                spdlog::warn("live hls exited (status {}), restarting in 5s", status);
                child_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        stop_child();
        if (running_ && !reload_requested_)
            std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
