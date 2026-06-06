#include "json_io.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "windows_lean.h"

namespace svg_mb_control {

namespace {

// Builds a per-process, per-call unique temp suffix so concurrent writers
// (multiple processes, or even multiple threads inside one process) targeting
// the same JSON file never clobber each other's in-flight temp file.
std::string MakeUniqueTempSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t seq = counter.fetch_add(1u, std::memory_order_relaxed);
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = 0u;
#endif
    return ".tmp." + std::to_string(pid) + "." + std::to_string(seq);
}

void ReplaceFileWithTemp(const std::filesystem::path& temp,
                         const std::filesystem::path& target) {
#ifdef _WIN32
    DWORD error = 0u;
    for (unsigned int attempt = 0u; attempt < 6u; ++attempt) {
        if (MoveFileExW(temp.wstring().c_str(),
                        target.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        error = GetLastError();
        const bool retryable = error == ERROR_ACCESS_DENIED ||
                               error == ERROR_SHARING_VIOLATION ||
                               error == ERROR_LOCK_VIOLATION;
        if (!retryable || attempt == 5u) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10u << attempt));
    }

    std::error_code cleanup_ec;
    std::filesystem::remove(temp, cleanup_ec);
    throw std::runtime_error(
        "Failed to replace JSON output file " + target.string() +
        ": Windows error " + std::to_string(error));
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        throw std::runtime_error("Failed to rename JSON temp file: " +
                                 ec.message());
    }
#endif
}

}  // namespace

std::string JsonStringOr(const nlohmann::json& value,
                         std::string_view key,
                         std::string_view fallback) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::string(fallback);
}

std::uint32_t JsonUInt32Or(const nlohmann::json& value,
                           std::string_view key,
                           std::uint32_t fallback) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_number_unsigned()) {
        return found->get<std::uint32_t>();
    }
    if (found != value.end() && found->is_number_integer()) {
        const auto raw = found->get<std::int64_t>();
        if (raw >= 0 && raw <= static_cast<std::int64_t>(UINT32_MAX)) {
            return static_cast<std::uint32_t>(raw);
        }
    }
    return fallback;
}

bool JsonBoolOr(const nlohmann::json& value,
                std::string_view key,
                bool fallback) {
    const auto found = value.find(std::string(key));
    return found != value.end() && found->is_boolean()
               ? found->get<bool>()
               : fallback;
}

nlohmann::json MakeSchemaObject(std::uint32_t schema_version) {
    nlohmann::json payload = nlohmann::json::object();
    payload["schema_version"] = schema_version;
    return payload;
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path,
                            std::string_view contract_name) {
#ifdef _WIN32
    // Open with FILE_SHARE_DELETE so a concurrent atomic-rename writer
    // (WriteJsonFileAtomic + MoveFileExW with MOVEFILE_REPLACE_EXISTING) is
    // never blocked by an in-flight reader. Without this, std::ifstream's
    // default share mode (no SHARE_DELETE) made the writer fail with
    // ERROR_ACCESS_DENIED (Windows error 5) any time --status / --health /
    // ReadPendingWrites raced a worker tick.
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        throw std::runtime_error("Could not open " +
                                 std::string(contract_name) + ": " +
                                 path.string() + " (Windows error " +
                                 std::to_string(err) + ")");
    }

    std::string content;
    {
        constexpr DWORD kChunk = 64u * 1024u;
        std::vector<char> buffer(kChunk);
        for (;;) {
            DWORD bytes_read = 0u;
            if (!ReadFile(handle, buffer.data(), kChunk, &bytes_read,
                          nullptr)) {
                const DWORD err = GetLastError();
                CloseHandle(handle);
                throw std::runtime_error("Failed reading " +
                                         std::string(contract_name) + ": " +
                                         path.string() + " (Windows error " +
                                         std::to_string(err) + ")");
            }
            if (bytes_read == 0u) {
                break;
            }
            content.append(buffer.data(), bytes_read);
        }
    }
    CloseHandle(handle);

    try {
        return nlohmann::json::parse(content);
    } catch (const nlohmann::json::parse_error& error) {
        throw std::runtime_error("JSON parse error in " +
                                 std::string(contract_name) + " " +
                                 path.string() + ": " + error.what());
    }
#else
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open " +
                                 std::string(contract_name) + ": " +
                                 path.string());
    }

    try {
        return nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error& error) {
        throw std::runtime_error("JSON parse error in " +
                                 std::string(contract_name) + " " +
                                 path.string() + ": " + error.what());
    }
#endif
}

void WriteJsonFileAtomic(const std::filesystem::path& target_path,
                         const nlohmann::json& payload,
                         int indent) {
    if (target_path.empty()) {
        throw std::runtime_error("Cannot write JSON to an empty path.");
    }

    std::error_code ec;
    const std::filesystem::path parent = target_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    if (ec) {
        throw std::runtime_error("Could not create JSON output directory: " +
                                 ec.message());
    }

    const std::filesystem::path temp =
        target_path.parent_path() /
        (target_path.filename().string() + MakeUniqueTempSuffix());
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            throw std::runtime_error("Could not open JSON temp file: " +
                                     temp.string());
        }
        stream << payload.dump(indent) << '\n';
        stream.flush();
        if (stream.fail()) {
            throw std::runtime_error("Failed writing JSON temp file: " +
                                     temp.string());
        }
    }

#ifdef _WIN32
    // Force data sectors to disk before the rename. Without this, a crash
    // after the rename can leave the target file backed by uncommitted
    // sectors that read back as whitespace.
    {
        HANDLE handle = CreateFileW(temp.wstring().c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            throw std::runtime_error(
                "Could not reopen JSON temp file for flush " + temp.string() +
                ": Windows error " + std::to_string(error));
        }
        const BOOL ok = FlushFileBuffers(handle);
        const DWORD flush_error = ok ? 0u : GetLastError();
        CloseHandle(handle);
        if (!ok) {
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            throw std::runtime_error("Failed to flush JSON temp file " +
                                     temp.string() + ": Windows error " +
                                     std::to_string(flush_error));
        }
    }
#endif

    ReplaceFileWithTemp(temp, target_path);
}

bool TryWriteJsonFileAtomic(const std::filesystem::path& target_path,
                            const nlohmann::json& payload,
                            int indent,
                            std::string* error_message) {
    try {
        WriteJsonFileAtomic(target_path, payload, indent);
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return false;
    }
}

std::optional<nlohmann::json> TryReadJsonObject(
    const std::filesystem::path& path,
    std::string_view contract_name) {
    try {
        nlohmann::json payload = ReadJsonFile(path, contract_name);
        if (payload.is_object()) {
            return payload;
        }
    } catch (const std::exception&) {
        // Missing/unreadable/malformed sidecars collapse to nullopt so
        // callers can boot before any writer has produced the file.
    }
    return std::nullopt;
}

}  // namespace svg_mb_control
