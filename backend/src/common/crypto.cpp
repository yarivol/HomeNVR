#include "crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace crypto {

namespace {
constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kKeyLen = 32;  // AES-256

void aes_gcm(bool encrypt_mode, const std::string& key, const unsigned char* iv,
             const unsigned char* in, size_t in_len, unsigned char* out,
             unsigned char* tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    const EVP_CIPHER* cipher = EVP_aes_256_gcm();
    if (encrypt_mode) {
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr,
                               reinterpret_cast<const unsigned char*>(key.data()), iv) != 1)
            throw std::runtime_error("EVP_EncryptInit_ex failed");
        int len = 0, total = 0;
        if (EVP_EncryptUpdate(ctx, out, &len, in, static_cast<int>(in_len)) != 1)
            throw std::runtime_error("EVP_EncryptUpdate failed");
        total = len;
        if (EVP_EncryptFinal_ex(ctx, out + total, &len) != 1)
            throw std::runtime_error("EVP_EncryptFinal_ex failed");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag) != 1)
            throw std::runtime_error("GCM get tag failed");
    } else {
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr,
                               reinterpret_cast<const unsigned char*>(key.data()), iv) != 1)
            throw std::runtime_error("EVP_DecryptInit_ex failed");
        int len = 0, total = 0;
        if (EVP_DecryptUpdate(ctx, out, &len, in, static_cast<int>(in_len)) != 1)
            throw std::runtime_error("EVP_DecryptUpdate failed");
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, tag) != 1)
            throw std::runtime_error("GCM set tag failed");
        if (EVP_DecryptFinal_ex(ctx, out + total, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("GCM auth failed (wrong key or corrupted data)");
        }
    }
    EVP_CIPHER_CTX_free(ctx);
}
}  // namespace

std::string load_key_hex(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open secret key file: " + path);
    std::string hex;
    f >> hex;
    if (hex.size() != kKeyLen * 2)
        throw std::runtime_error("secret key must be 32 bytes hex (64 chars)");
    return hex;
}

std::string encrypt(const std::string& plaintext, const std::string& key_hex) {
    const std::string key = hex_to_bytes(key_hex);
    unsigned char iv[kIvLen];
    if (RAND_bytes(iv, kIvLen) != 1) throw std::runtime_error("RAND_bytes failed");

    std::vector<unsigned char> ct(plaintext.size());
    unsigned char tag[kTagLen];
    aes_gcm(true, key, iv, reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(), ct.data(), tag);

    std::string blob;
    blob.reserve(kIvLen + kTagLen + ct.size());
    blob.append(reinterpret_cast<char*>(iv), kIvLen);
    blob.append(reinterpret_cast<char*>(tag), kTagLen);
    blob.append(reinterpret_cast<char*>(ct.data()), ct.size());
    return base64_encode(reinterpret_cast<const unsigned char*>(blob.data()), blob.size());
}

std::string decrypt(const std::string& encoded, const std::string& key_hex) {
    const std::string key = hex_to_bytes(key_hex);
    const std::string blob = base64_decode(encoded);
    if (blob.size() < kIvLen + kTagLen)
        throw std::runtime_error("encrypted blob too short");

    const auto* iv  = reinterpret_cast<const unsigned char*>(blob.data());
    const auto* tag = reinterpret_cast<const unsigned char*>(blob.data() + kIvLen);
    const auto* ct  = reinterpret_cast<const unsigned char*>(blob.data() + kIvLen + kTagLen);
    const size_t ct_len = blob.size() - kIvLen - kTagLen;

    std::vector<unsigned char> pt(ct_len);
    unsigned char tag_copy[kTagLen];
    std::memcpy(tag_copy, tag, kTagLen);
    aes_gcm(false, key, iv, ct, ct_len, pt.data(), tag_copy);
    return std::string(reinterpret_cast<char*>(pt.data()), pt.size());
}

// ---------- base64 / hex ----------

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[v & 63] : '=';
    }
    return out;
}

std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string hex_to_bytes(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

}  // namespace crypto
