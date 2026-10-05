#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

inline std::string FileSha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD objectSize = 0, written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
        sizeof(objectSize), &written, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return {}; }
    std::vector<unsigned char> object(objectSize);
    std::string result;
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
        std::array<char, 65536> bytes{};
        bool ok = true;
        while (input.read(bytes.data(), bytes.size()) || input.gcount()) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(input.gcount()), 0) < 0) {
                ok = false; break;
            }
        }
        std::array<unsigned char, 32> digest{};
        if (ok && input.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
            std::ostringstream out;
            for (auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
            result = out.str();
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

inline std::filesystem::path ModulePath(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? std::filesystem::path(path.data()) : std::filesystem::path();
}

inline std::string HexBytes(const void* address, size_t size) {
    std::ostringstream out;
    const auto* bytes = static_cast<const unsigned char*>(address);
    for (size_t i = 0; i < size; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return out.str();
}
