#include "json_io.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace svg_mb_control {

namespace {

void ReplaceFileWithTemp(const std::filesystem::path& temp,
                         const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temp.wstring().c_str(),
                     target.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        throw std::runtime_error(
            "Failed to replace JSON output file: Windows error " +
            std::to_string(error));
    }
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

nlohmann::json MakeSchemaObject(std::uint32_t schema_version) {
    nlohmann::json payload = nlohmann::json::object();
    payload["schema_version"] = schema_version;
    return payload;
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path,
                            std::string_view contract_name) {
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
        (target_path.filename().string() + ".tmp");
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

    ReplaceFileWithTemp(temp, target_path);
}

bool TryWriteJsonFileAtomic(const std::filesystem::path& target_path,
                            const nlohmann::json& payload,
                            int indent) {
    try {
        WriteJsonFileAtomic(target_path, payload, indent);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace svg_mb_control
