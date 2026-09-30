#pragma once
// AES-256-GCM для секретов камеры (ТЗ §39).
// Ключ — 32 байта в hex-файле (Docker secret, ТЗ §73.4).
// Формат результата: base64( iv[12] | tag[16] | ciphertext )
#include <string>

namespace crypto {

// Загружает hex-ключ из файла (openssl rand -hex 32)
std::string load_key_hex(const std::string& path);

std::string encrypt(const std::string& plaintext, const std::string& key_hex);
std::string decrypt(const std::string& encoded, const std::string& key_hex);

// Утилиты
std::string base64_encode(const unsigned char* data, size_t len);
std::string base64_decode(const std::string& in);
std::string hex_to_bytes(const std::string& hex);

}  // namespace crypto
