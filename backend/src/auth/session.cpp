#include "session.hpp"

#include <spdlog/spdlog.h>

namespace auth {

namespace {
// kSessionTtlSec — в session.hpp (общий для БД и cookie)

std::string extract_token(const crow::request& req) {
    const std::string cookie = req.get_header_value("Cookie");
    const std::string key = "homenvr_session=";
    const auto pos = cookie.find(key);
    if (pos == std::string::npos) return "";
    const auto start = pos + key.size();
    const auto end = cookie.find(';', start);
    return cookie.substr(start, end == std::string::npos ? end : end - start);
}
}  // namespace

std::string create_session(db::Database& db, int user_id) {
    return db.tx([&](pqxx::work& w) {
        // удаляем протухшие сессии заодно
        w.exec("DELETE FROM sessions WHERE expires_at < now()");
        const auto r = w.exec_params(
            "INSERT INTO sessions (user_id, expires_at) "
            "VALUES ($1, now() + make_interval(secs => $2)) RETURNING id",
            user_id, kSessionTtlSec);
        return r[0]["id"].as<std::string>();
    });
}

void destroy_session(db::Database& db, const std::string& token) {
    if (token.empty()) return;
    db.tx([&](pqxx::work& w) {
        w.exec_params("DELETE FROM sessions WHERE id = $1", token);
    });
}

std::optional<AuthUser> authenticate(const crow::request& req, db::Database& db) {
    const std::string token = extract_token(req);
    if (token.empty()) return std::nullopt;

    try {
        return db.tx([&](pqxx::work& w) -> std::optional<AuthUser> {
            const auto r = w.exec_params(
                "SELECT u.id, u.username, r.name AS role "
                "FROM sessions s "
                "JOIN users u ON u.id = s.user_id "
                "JOIN roles r ON r.id = u.role_id "
                "WHERE s.id = $1 AND s.expires_at > now() AND u.enabled",
                token);
            if (r.empty()) return std::nullopt;
            return AuthUser{r[0]["id"].as<int>(),
                            r[0]["username"].as<std::string>(),
                            r[0]["role"].as<std::string>()};
        });
    } catch (const std::exception& e) {
        spdlog::error("auth lookup failed: {}", e.what());
        return std::nullopt;
    }
}

std::optional<AuthUser> require_user(const crow::request& req, db::Database& db) {
    return authenticate(req, db);
}

std::optional<AuthUser> require_admin(const crow::request& req, db::Database& db) {
    auto user = authenticate(req, db);
    if (!user || user->role != "ADMIN") return std::nullopt;
    return user;
}

std::string session_cookie(const std::string& token, int max_age_sec) {
    return "homenvr_session=" + token +
           "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + std::to_string(max_age_sec);
}

std::string clear_session_cookie() {
    return "homenvr_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
}

}  // namespace auth
