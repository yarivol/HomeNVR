#pragma once
// Сессии и проверка доступа (ТЗ §42, §67).
// Cookie: HttpOnly + SameSite=Lax. Флаг Secure добавится при включении HTTPS (ТЗ §73.6).
#include <crow.h>

#include <optional>
#include <string>

#include "../db/database.hpp"

namespace auth {

// Время жизни сессии: 7 дней (ТЗ §73.8). Одно место — используется и для
// expires_at в БД, и для Max-Age cookie.
inline constexpr int kSessionTtlSec = 7 * 24 * 3600;

struct AuthUser {
    int id;
    std::string username;
    std::string role;  // "USER" | "ADMIN"
};

// Создаёт сессию, возвращает токен (uuid)
std::string create_session(db::Database& db, int user_id);

// Удаляет сессию
void destroy_session(db::Database& db, const std::string& token);

// Достаёт пользователя из cookie "homenvr_session". nullopt — не авторизован.
std::optional<AuthUser> authenticate(const crow::request& req, db::Database& db);

// Хелперы для handlers: возвращают готовый 401/403, если доступа нет.
// Использование: auto user = require_user(req, db); if (!user) return deny();
std::optional<AuthUser> require_user(const crow::request& req, db::Database& db);
std::optional<AuthUser> require_admin(const crow::request& req, db::Database& db);

// Заголовок Set-Cookie для установки/сброса сессии
std::string session_cookie(const std::string& token, int max_age_sec);
std::string clear_session_cookie();

}  // namespace auth
