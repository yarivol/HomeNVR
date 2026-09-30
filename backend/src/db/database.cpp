#include "database.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace db {

Database::Database(std::string url) : url_(std::move(url)) {}

void Database::connect_with_retry(int attempts, int delay_sec) {
    for (int i = 1; i <= attempts; ++i) {
        try {
            conn_ = std::make_unique<pqxx::connection>(url_);
            spdlog::info("database connected");
            return;
        } catch (const std::exception& e) {
            spdlog::warn("database not ready (attempt {}/{}): {}", i, attempts, e.what());
            std::this_thread::sleep_for(std::chrono::seconds(delay_sec));
        }
    }
    throw std::runtime_error("cannot connect to database");
}

void Database::run_migrations(const std::string& dir) {
    tx([](pqxx::work& w) {
        w.exec(R"(
            CREATE TABLE IF NOT EXISTS schema_migrations (
                name       TEXT PRIMARY KEY,
                applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
            )
        )");
    });

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".sql") files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        const std::string name = file.filename().string();
        const bool applied = tx([&](pqxx::work& w) {
            return !w.exec_params("SELECT 1 FROM schema_migrations WHERE name = $1", name).empty();
        });
        if (applied) continue;

        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();

        spdlog::info("applying migration {}", name);
        tx([&](pqxx::work& w) {
            w.exec(ss.str());
            w.exec_params("INSERT INTO schema_migrations (name) VALUES ($1)", name);
        });
        spdlog::info("migration {} applied", name);
    }
}

}  // namespace db
