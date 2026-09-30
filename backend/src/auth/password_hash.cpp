#include "password_hash.hpp"

#include <argon2.h>
#include <openssl/rand.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace auth {

namespace {
// Параметры argon2id: баланс безопасность/нагрузка для домашнего сервера
constexpr std::uint32_t kTimeCost   = 3;
constexpr std::uint32_t kMemoryKiB  = 64 * 1024;  // 64 МБ
constexpr std::uint32_t kThreads    = 2;
constexpr std::uint32_t kSaltLen    = 16;
constexpr std::uint32_t kHashLen    = 32;
constexpr size_t kEncodedLen        = 256;
}  // namespace

std::string hash_password(const std::string& password) {
    if (password.size() < 8)
        throw std::invalid_argument("password too short (min 8 chars)");
    if (password.size() > 128)
        throw std::invalid_argument("password too long (max 128 chars)");

    unsigned char salt[kSaltLen];
    if (RAND_bytes(salt, kSaltLen) != 1)
        throw std::runtime_error("RAND_bytes failed");

    std::vector<char> encoded(kEncodedLen);
    const int rc = argon2id_hash_encoded(
        kTimeCost, kMemoryKiB, kThreads,
        password.data(), password.size(),
        salt, kSaltLen, kHashLen,
        encoded.data(), encoded.size());
    if (rc != ARGON2_OK)
        throw std::runtime_error(std::string("argon2 failed: ") + argon2_error_message(rc));

    return std::string(encoded.data());
}

bool verify_password(const std::string& encoded_hash, const std::string& password) {
    if (password.empty() || password.size() > 128) return false;
    return argon2id_verify(encoded_hash.c_str(), password.data(), password.size()) == ARGON2_OK;
}

}  // namespace auth
