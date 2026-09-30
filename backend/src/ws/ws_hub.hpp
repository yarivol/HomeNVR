#pragma once
// WebSocket-хаб: хранит подключения и рассылает события (ТЗ §41).
#include <crow.h>

#include <mutex>
#include <string>
#include <unordered_set>

class WsHub {
public:
    void add(crow::websocket::connection* conn) {
        std::lock_guard lock(mutex_);
        conns_.insert(conn);
    }

    void remove(crow::websocket::connection* conn) {
        std::lock_guard lock(mutex_);
        conns_.erase(conn);
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return conns_.size();
    }

    // event — имя события ("camera.connected"), payload — JSON-объект строкой
    void broadcast(const std::string& event, const std::string& payload = "{}") {
        const std::string msg = "{\"event\":\"" + event + "\",\"data\":" + payload + "}";
        std::lock_guard lock(mutex_);
        for (auto* conn : conns_) conn->send_text(msg);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<crow::websocket::connection*> conns_;
};
