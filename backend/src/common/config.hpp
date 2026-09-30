#pragma once
// Конфигурация backend'а из переменных окружения (docker-compose)
#include <cstdint>
#include <cstdlib>
#include <string>

struct Config {
    std::string database_url;
    std::string recordings_path = "/data/recordings";
    std::string thumbnails_path = "/data/thumbnails";
    std::string exports_path    = "/data/exports";
    std::string live_path       = "/data/live";   // HLS live-поток
    std::string hls_path        = "/data/hls";    // HLS-сессии архива
    std::string logs_path       = "/data/logs";   // файловые логи (ротация)
    std::string secret_key_file;
    std::string migrations_path = "/app/db/migrations";
    std::uint16_t port = 8080;

    static Config from_env() {
        Config c;
        auto env = [](const char* name) -> std::string {
            const char* v = std::getenv(name);
            return v ? v : "";
        };
        c.database_url     = env("DATABASE_URL");
        if (auto v = env("RECORDINGS_PATH"); !v.empty()) c.recordings_path = v;
        if (auto v = env("THUMBNAILS_PATH"); !v.empty()) c.thumbnails_path = v;
        if (auto v = env("EXPORTS_PATH");    !v.empty()) c.exports_path = v;
        if (auto v = env("LIVE_PATH");       !v.empty()) c.live_path = v;
        if (auto v = env("HLS_PATH");        !v.empty()) c.hls_path = v;
        if (auto v = env("LOGS_PATH");       !v.empty()) c.logs_path = v;
        c.secret_key_file  = env("SECRET_KEY_FILE");
        if (auto v = env("MIGRATIONS_PATH"); !v.empty()) c.migrations_path = v;
        return c;
    }
};
