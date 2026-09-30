#include "camera_manager.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "rtsp_probe.hpp"

namespace {
constexpr int kCheckIntervalSec = 30;    // мониторинг живого потока
constexpr int kReconnectBaseSec = 5;     // стартовый backoff
constexpr int kReconnectMaxSec  = 60;    // максимальный backoff
}  // namespace

std::string CameraManager::state_str() const {
    switch (state_) {
        case CameraState::Connected:    return "connected";
        case CameraState::Reconnecting: return "reconnecting";
        default:                        return "disconnected";
    }
}

void CameraManager::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
    spdlog::info("camera manager started");
}

void CameraManager::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void CameraManager::reload() {
    reload_requested_ = true;
}

void CameraManager::set_state(CameraState s) {
    const CameraState prev = state_.exchange(s);
    if (prev == s) return;

    const std::string event = "camera." + state_str();
    // человекочитаемый переход состояния
    static const char* names[] = {"disconnected", "reconnecting", "connected"};
    const auto name_of = [](CameraState st) { return names[static_cast<int>(st)]; };
    spdlog::info("camera state: {} -> {}", name_of(prev), name_of(s));
    if (event_fn_) event_fn_(event, "{}");
}

std::string CameraManager::current_url() {
    std::lock_guard lock(mutex_);
    if (!url_provider_) return "";
    try {
        return url_provider_();
    } catch (const std::exception& e) {
        spdlog::error("camera url provider failed: {}", e.what());
        return "";
    }
}

void CameraManager::interruptible_sleep(int seconds) {
    for (int i = 0; i < seconds * 10 && running_ && !reload_requested_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void CameraManager::run() {
    int backoff = kReconnectBaseSec;

    while (running_) {
        if (reload_requested_.exchange(false)) {
            spdlog::info("camera settings changed, reconnecting");
            set_state(CameraState::Disconnected);
        }

        const std::string url = current_url();
        if (url.empty()) {
            // камера ещё не настроена (setup wizard не завершён)
            set_state(CameraState::Disconnected);
            interruptible_sleep(5);
            continue;
        }

        if (rtsp::probe(url)) {
            backoff = kReconnectBaseSec;
            set_state(CameraState::Connected);
            interruptible_sleep(kCheckIntervalSec);
        } else {
            set_state(CameraState::Disconnected);
            spdlog::warn("camera lost, reconnecting in {}s", backoff);
            set_state(CameraState::Reconnecting);
            interruptible_sleep(backoff);
            backoff = std::min(backoff * 2, kReconnectMaxSec);
        }
    }
    set_state(CameraState::Disconnected);
}
