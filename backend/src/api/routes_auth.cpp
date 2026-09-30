// Auth API: login / logout / me (ТЗ §42).
// Защита от перебора: после 5 неудачных попыток — блокировка на 60 сек.
#include "routes.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "../auth/password_hash.hpp"
#include "../auth/session.hpp"

namespace api {
namespace {

// Простой rate limiter по IP (в памяти процесса)
struct LoginAttempts {
    int fails = 0;
    std::chrono::steady_clock::time_point locked_until{};
};

std::mutex g_limiter_mutex;
std::unordered_map<std::string, LoginAttempts> g_attempts;

bool is_locked(const std::string& ip) {
    std::lock_guard lock(g_limiter_mutex);
    auto it = g_attempts.find(ip);
    return it != g_attempts.end() &&
           std::chrono::steady_clock::now() < it->second.locked_until;
}

void register_fail(const std::string& ip) {
    std::lock_guard lock(g_limiter_mutex);
    auto& a = g_attempts[ip];
    if (++a.fails >= 5) {
        a.fails = 0;
        a.locked_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        spdlog::warn("login rate limit triggered");
    }
}

void clear_fails(const std::string& ip) {
    std::lock_guard lock(g_limiter_mutex);
    g_attempts.erase(ip);
}

crow::response json_error(int code, const std::string& msg) {
    crow::json::wvalue res;
    res["error"] = msg;
    return crow::response(code, res);
}

}  // namespace

void register_auth_routes(crow::SimpleApp& app, db::Database& db) {
    // POST /api/auth/login {username, password}
    CROW_ROUTE(app, "/api/auth/login").methods(crow::HTTPMethod::POST)([&db](const crow::request& req) {
        const std::string ip = req.get_header_value("X-Real-IP");
        if (is_locked(ip))
            return json_error(429, "Слишком много попыток. Попробуйте позже.");

        const auto body = crow::json::load(req.body);
        if (!body || !body.has("username") || !body.has("password"))
            return json_error(400, "username and password required");

        const std::string username = body["username"].s();
        const std::string password = body["password"].s();
        if (username.size() > 64 || password.size() > 128)
            return json_error(400, "invalid input");

        // ищем пользователя
        auto user = db.tx([&](pqxx::work& w) -> std::optional<std::tuple<int, std::string, bool>> {
            const auto r = w.exec_params(
                "SELECT id, password_hash, enabled FROM users WHERE username = $1", username);
            if (r.empty()) return std::nullopt;
            return std::make_tuple(r[0]["id"].as<int>(),
                                   r[0]["password_hash"].as<std::string>(),
                                   r[0]["enabled"].as<bool>());
        });

        bool ok = false;
        if (user && std::get<2>(*user))
            ok = auth::verify_password(std::get<1>(*user), password);

        if (!ok) {
            register_fail(ip);
            spdlog::warn("login failed for user '{}'", username);
            return json_error(401, "Неверный логин или пароль");
        }

        clear_fails(ip);
        const std::string token = auth::create_session(db, std::get<0>(*user));
        spdlog::info("user '{}' logged in", username);

        crow::json::wvalue res;
        res["ok"] = true;
        crow::response resp(200, res);
        resp.add_header("Set-Cookie", auth::session_cookie(token, 7 * 24 * 3600));
        return resp;
    });

    // POST /api/auth/logout
    CROW_ROUTE(app, "/api/auth/logout").methods(crow::HTTPMethod::POST)([&db](const crow::request& req) {
        const std::string cookie = req.get_header_value("Cookie");
        const std::string key = "homenvr_session=";
        if (const auto pos = cookie.find(key); pos != std::string::npos) {
            const auto start = pos + key.size();
            const auto end = cookie.find(';', start);
            auth::destroy_session(db, cookie.substr(start, end == std::string::npos ? end : end - start));
        }
        crow::json::wvalue res;
        res["ok"] = true;
        crow::response resp(200, res);
        resp.add_header("Set-Cookie", auth::clear_session_cookie());
        return resp;
    });

    // GET /api/auth/me
    CROW_ROUTE(app, "/api/auth/me")([&db](const crow::request& req) {
        const auto user = auth::authenticate(req, db);
        if (!user) return json_error(401, "unauthorized");
        crow::json::wvalue res;
        res["id"] = user->id;
        res["username"] = user->username;
        res["role"] = user->role;
        return crow::response(200, res);
    });
}

}  // namespace api
