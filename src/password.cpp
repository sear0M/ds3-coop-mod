#include "password.h"

#include <windows.h>

#include <bcrypt.h>

#include <cstdio>

namespace password {
namespace {

// SHA-256 of the menu password, lowercase hex. To change the password, put the new hash here:
//   PowerShell:  (Get-FileHash -Algorithm SHA256 -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes('NOVA_SENHA')))).Hash.ToLower()
constexpr const char* kPasswordSha256 = "d7ff1abfe9621fb0924da85f3cc4863ad47329b3dfde8ada62ea889747168e9b";

bool Sha256Hex(const std::string& input, char out[65]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
              BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                             static_cast<ULONG>(input.size()), 0) >= 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i) std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
    return true;
}

}  // namespace

bool Check(const std::string& input) {
    char hex[65];
    if (!Sha256Hex(input, hex)) return false;
    unsigned char diff = 0;
    for (int i = 0; i < 64; ++i) diff |= static_cast<unsigned char>(hex[i] ^ kPasswordSha256[i]);
    SecureZeroMemory(hex, sizeof(hex));
    return diff == 0;
}

}  // namespace password
