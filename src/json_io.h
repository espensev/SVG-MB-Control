#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace svg_mb_control {

nlohmann::json MakeSchemaObject(std::uint32_t schema_version);

nlohmann::json ReadJsonFile(const std::filesystem::path& path,
                            std::string_view contract_name);

void WriteJsonFileAtomic(const std::filesystem::path& target_path,
                         const nlohmann::json& payload,
                         int indent = 2);

bool TryWriteJsonFileAtomic(const std::filesystem::path& target_path,
                            const nlohmann::json& payload,
                            int indent = 2);

}  // namespace svg_mb_control
