#include "pawnio_binary.h"

#include "env_util.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace svg_mb_control {

namespace {

constexpr std::uint32_t kPawnIoLoadBinary = (41394u << 16) | (0x821u << 2);
constexpr std::size_t kPawnIoMaxBinaryBytes = 1024u * 1024u;
constexpr std::size_t kSha256DigestBytes = 32u;
constexpr const char* kSkipHashEnvVar = "SVG_MB_CONTROL_PAWNIO_SKIP_HASH";

std::filesystem::path CurrentExecutableDirectory() {
    std::array<char, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(
               std::string(buffer.data(), buffer.data() + length))
        .parent_path();
}

void AddUniquePath(std::vector<std::filesystem::path>& paths,
                   const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }
    const std::filesystem::path normalized = candidate.lexically_normal();
    if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
        paths.push_back(normalized);
    }
}

void AddRootAndParents(std::vector<std::filesystem::path>& paths,
                       std::filesystem::path root,
                       std::size_t max_depth) {
    for (std::size_t depth = 0u; depth < max_depth && !root.empty(); ++depth) {
        AddUniquePath(paths, root);
        const std::filesystem::path parent = root.parent_path();
        if (parent == root) {
            break;
        }
        root = parent;
    }
}

PawnIoStatus StatusFromWin32Error(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_SERVICE_DOES_NOT_EXIST:
        case ERROR_DEV_NOT_EXIST:
            return PawnIoStatus::not_found;
        case ERROR_ACCESS_DENIED:
            return PawnIoStatus::access_denied;
        case ERROR_NOT_SUPPORTED:
            return PawnIoStatus::not_supported;
        default:
            return PawnIoStatus::error;
    }
}

std::string ToLowerHex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2u);
    for (std::size_t i = 0u; i < size; ++i) {
        out[i * 2u] = kHex[(data[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = kHex[data[i] & 0xFu];
    }
    return out;
}

std::string ToLowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool SkipHashRequestedByEnv() {
    const std::string value = GetEnvOrDefault(kSkipHashEnvVar, "");
    return value == "1" || value == "true" || value == "TRUE";
}

PawnIoStatus ReadFileBytes(const std::filesystem::path& path,
                           std::vector<std::uint8_t>* out_buffer) {
    HANDLE file_handle = CreateFileA(
        path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return StatusFromWin32Error(GetLastError());
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size) || file_size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) > kPawnIoMaxBinaryBytes) {
        CloseHandle(file_handle);
        return PawnIoStatus::error;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size.QuadPart));
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file_handle, buffer.data(),
                                  static_cast<DWORD>(buffer.size()),
                                  &bytes_read, nullptr);
    CloseHandle(file_handle);
    if (!read_ok || bytes_read != buffer.size()) {
        return PawnIoStatus::error;
    }
    *out_buffer = std::move(buffer);
    return PawnIoStatus::ok;
}

}  // namespace

// SHA-256 of the SMN-only bin currently shipped under resources/pawnio/.
// Computed via Get-FileHash -Algorithm SHA256 against the committed file. Keep
// the hash here in sync with the bytes in resources/pawnio/AMDFamily17.bin;
// CI verification of that pairing lives in the pawnio_binary tests.
const PawnIoBinarySpec kPawnIoSpecAmdFamily17V1{
    "AMDFamily17.bin",
    "099dc01d6db97ea997fec4a461e191cc64b9d7ce47c9d2153c451c56c2adcf50",
    "AMDFamily17 (SMN-only, v1)",
    PawnIoVerification::warn_only,
};

const char* PawnIoStatusString(PawnIoStatus status) {
    switch (status) {
        case PawnIoStatus::ok: return "ok";
        case PawnIoStatus::error: return "error";
        case PawnIoStatus::invalid_arg: return "invalid_arg";
        case PawnIoStatus::not_supported: return "not_supported";
        case PawnIoStatus::not_found: return "not_found";
        case PawnIoStatus::access_denied: return "access_denied";
        case PawnIoStatus::hash_mismatch: return "hash_mismatch";
        default: return "unknown";
    }
}

std::filesystem::path ResolvePawnIoBinaryPath(const PawnIoBinarySpec& spec) {
    if (spec.filename.empty()) {
        return {};
    }

    const std::array<const char*, 2> env_names = {
        "SVG_MB_CONTROL_PAWNIO_BIN",
        "SVG_MB_PAWNIO_BIN",
    };
    for (const char* env_name : env_names) {
        const std::string env_value = GetEnvOrDefault(env_name, "");
        if (env_value.empty()) {
            continue;
        }
        const std::filesystem::path candidate(env_value);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    std::vector<std::filesystem::path> search_roots;
    AddRootAndParents(search_roots, CurrentExecutableDirectory(), 6u);

    std::error_code cwd_ec;
    AddRootAndParents(search_roots, std::filesystem::current_path(cwd_ec), 6u);

    const std::filesystem::path filename(spec.filename);
    for (const auto& root : search_roots) {
        for (const auto& candidate : std::array<std::filesystem::path, 3>{
                 root / "resources" / "pawnio" / filename,
                 root / "release" / "resources" / "pawnio" / filename,
                 root / "dist" / "resources" / "pawnio" / filename,
             }) {
            std::error_code exists_ec;
            if (std::filesystem::exists(candidate, exists_ec) &&
                !std::filesystem::is_directory(candidate, exists_ec)) {
                return std::filesystem::absolute(candidate).lexically_normal();
            }
        }
    }

    return {};
}

bool ComputeSha256Hex(const std::uint8_t* data,
                      std::size_t size,
                      std::string* out_hex) {
    if (out_hex == nullptr || (data == nullptr && size != 0u)) {
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    std::array<std::uint8_t, kSha256DigestBytes> digest{};

    if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        bool hash_ok = true;
        if (size != 0u) {
            hash_ok = BCRYPT_SUCCESS(BCryptHashData(
                hash,
                const_cast<PUCHAR>(static_cast<const UCHAR*>(data)),
                static_cast<ULONG>(size), 0));
        }
        if (hash_ok &&
            BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(),
                                            static_cast<ULONG>(digest.size()),
                                            0))) {
            *out_hex = ToLowerHex(digest.data(), digest.size());
            ok = true;
        }
        BCryptDestroyHash(hash);
    }

    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

PawnIoStatus LoadPawnIoBinary(HANDLE device_handle,
                              const PawnIoBinarySpec& spec,
                              const std::filesystem::path& bin_path,
                              std::string* out_warning) {
    if (out_warning != nullptr) {
        out_warning->clear();
    }
    if (device_handle == nullptr || device_handle == INVALID_HANDLE_VALUE ||
        bin_path.empty()) {
        return PawnIoStatus::invalid_arg;
    }

    std::vector<std::uint8_t> buffer;
    const PawnIoStatus read_status = ReadFileBytes(bin_path, &buffer);
    if (read_status != PawnIoStatus::ok) {
        return read_status;
    }

    PawnIoVerification mode = spec.verification;
    if (SkipHashRequestedByEnv()) {
        mode = PawnIoVerification::skip;
    }
    if (spec.expected_sha256_hex.empty()) {
        mode = PawnIoVerification::skip;
    }

    if (mode != PawnIoVerification::skip) {
        std::string actual_hex;
        if (!ComputeSha256Hex(buffer.data(), buffer.size(), &actual_hex)) {
            if (mode == PawnIoVerification::strict) {
                return PawnIoStatus::error;
            }
            if (out_warning != nullptr) {
                *out_warning = std::string(spec.display_name) +
                               " sha256 could not be computed; loading anyway";
            }
        } else if (actual_hex != ToLowerCopy(spec.expected_sha256_hex)) {
            if (mode == PawnIoVerification::strict) {
                if (out_warning != nullptr) {
                    *out_warning = std::string(spec.display_name) +
                                   " sha256 mismatch (expected " +
                                   std::string(spec.expected_sha256_hex) +
                                   ", got " + actual_hex + ")";
                }
                return PawnIoStatus::hash_mismatch;
            }
            if (out_warning != nullptr) {
                *out_warning = std::string(spec.display_name) +
                               " sha256 mismatch (expected " +
                               std::string(spec.expected_sha256_hex) +
                               ", got " + actual_hex +
                               "); loading anyway under warn_only";
            }
        }
    }

    DWORD bytes_returned = 0;
    const BOOL ioctl_ok = DeviceIoControl(
        device_handle, kPawnIoLoadBinary, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &bytes_returned, nullptr);
    if (!ioctl_ok) {
        return StatusFromWin32Error(GetLastError());
    }
    return PawnIoStatus::ok;
}

}  // namespace svg_mb_control
