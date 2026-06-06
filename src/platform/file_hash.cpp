#include "file_hash.h"

#include "windows_lean.h"

#include <bcrypt.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace svg_mb_control {

std::string Sha256FileHex(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }

    // Use permissive sharing so hashing config/log artifacts does not block a
    // concurrent runtime rotate or replacement.
    HANDLE file_handle = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const auto cleanup = [&]() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        CloseHandle(file_handle);
    };

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return {};
    }

    DWORD object_length = 0;
    DWORD result_length = 0;
    status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
        &result_length, 0);
    if (!BCRYPT_SUCCESS(status) || object_length == 0u) {
        cleanup();
        return {};
    }

    DWORD hash_length = 0;
    status = BCryptGetProperty(
        algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
        &result_length, 0);
    if (!BCRYPT_SUCCESS(status) || hash_length == 0u) {
        cleanup();
        return {};
    }

    std::vector<UCHAR> hash_object(object_length);
    std::vector<UCHAR> digest(hash_length);
    status = BCryptCreateHash(
        algorithm, &hash, hash_object.data(),
        static_cast<ULONG>(hash_object.size()), nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return {};
    }

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD bytes_read = 0u;
        const BOOL read_ok = ReadFile(
            file_handle, buffer.data(),
            static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
        if (!read_ok) {
            cleanup();
            return {};
        }
        if (bytes_read == 0u) {
            break;
        }
        status = BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(buffer.data()),
            static_cast<ULONG>(bytes_read), 0);
        if (!BCRYPT_SUCCESS(status)) {
            cleanup();
            return {};
        }
    }

    status = BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return {};
    }

    cleanup();

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (UCHAR byte : digest) {
        hex << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return hex.str();
}

}  // namespace svg_mb_control
