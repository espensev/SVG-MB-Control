#include "json_io.h"

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace svg_mb_control {

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

    std::filesystem::rename(temp, target_path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("Failed to rename JSON temp file: " +
                                 ec.message());
    }
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
