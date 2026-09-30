#pragma once
// Хеширование паролей: argon2id (ТЗ §39, §73.4).
#include <string>

namespace auth {

// Возвращает encoded-хеш (включает соль и параметры)
std::string hash_password(const std::string& password);

// Проверка пароля против encoded-хеша
bool verify_password(const std::string& encoded_hash, const std::string& password);

}  // namespace auth
