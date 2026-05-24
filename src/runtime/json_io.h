#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svg_mb_control {

nlohmann::json MakeSchemaObject(std::uint32_t schema_version);

// Defensive accessors for optional JSON object members. Each returns the
// typed value when `key` is present with a compatible type, otherwise
// `fallback`. JsonUInt32Or accepts a non-negative signed integer that fits in
// uint32 (including 0); this single semantic replaces three formerly
// divergent copies.
std::string JsonStringOr(const nlohmann::json& value,
                         std::string_view key,
                         std::string_view fallback = {});

std::uint32_t JsonUInt32Or(const nlohmann::json& value,
                           std::string_view key,
                           std::uint32_t fallback = 0u);

bool JsonBoolOr(const nlohmann::json& value,
                std::string_view key,
                bool fallback = false);

nlohmann::json ReadJsonFile(const std::filesystem::path& path,
                            std::string_view contract_name);

// Reads `path` and returns the parsed object on success, or std::nullopt when
// the file is missing, unreadable, not valid JSON, or not a JSON object.
// Collapses the recurring try/catch + is_object() guard used by sidecar
// readers that boot before any worker has produced their first file.
std::optional<nlohmann::json> TryReadJsonObject(
    const std::filesystem::path& path,
    std::string_view contract_name);

void WriteJsonFileAtomic(const std::filesystem::path& target_path,
                         const nlohmann::json& payload,
                         int indent = 2);

bool TryWriteJsonFileAtomic(const std::filesystem::path& target_path,
                            const nlohmann::json& payload,
                            int indent = 2);

}  // namespace svg_mb_control
