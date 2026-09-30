// HomeNVR backend — точка входа
// Phase 1: инфраструктурный skeleton (health check, status, WS-заглушка)
#include <crow.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace {

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    const std::string db_url = env_or("DATABASE_URL", "");
    spdlog::info("HomeNVR backend starting");
    spdlog::info("database: {}", db_url.empty() ? "NOT CONFIGURED" : "configured");

    crow::SimpleApp app;

    // Health check для Docker / мониторинга (ТЗ §59)
    CROW_ROUTE(app, "/health")([] {
        crow::json::wvalue res;
        res["status"] = "ok";
        return res;
    });

    // Статус системы (ТЗ §50) — пока заглушка, заполним в Phase 2-3
    CROW_ROUTE(app, "/api/system/status")([] {
        crow::json::wvalue res;
        res["backend"] = "ok";
        res["database"] = "unknown";
        res["camera"] = "unknown";
        res["recording"] = "stopped";
        res["motion"] = "disabled";
        res["storage_percent"] = 0;
        res["uptime_sec"] = 0;
        return res;
    });

    // WebSocket (ТЗ §41) — заглушка: принимает соединение, логирует
    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([](crow::websocket::connection& conn) {
            spdlog::info("ws client connected");
            conn.send_text(R"({"event":"connected"})");
        })
        .onclose([](crow::websocket::connection&, const std::string& reason, uint16_t) {
            spdlog::info("ws client disconnected: {}", reason);
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) {});

    constexpr std::uint16_t kPort = 8080;
    spdlog::info("listening on port {}", kPort);
    app.port(kPort).multithreaded().run();
    return 0;
}
