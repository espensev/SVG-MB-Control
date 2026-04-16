#include "runtime_write_policy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace svg_mb_control {

namespace {

std::size_t SkipWs(const std::string& text,
                   std::size_t offset,
                   std::size_t limit) {
    while (offset < limit &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
        ++offset;
    }
    return offset;
}

std::string ReadEntireFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open runtime policy: " +
                                 path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool FindObjectRange(const std::string& text,
                     std::string_view key,
                     std::size_t range_begin,
                     std::size_t range_end,
                     std::size_t* out_begin,
                     std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return false;
    }
    const std::size_t value_start = SkipWs(text, colon + 1u, range_end);
    if (value_start >= range_end || text[value_start] != '{') {
        return false;
    }

    std::size_t depth = 1u;
    std::size_t cursor = value_start + 1u;
    bool in_string = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_string) {
            if (ch == '\\' && cursor + 1u < range_end) {
                cursor += 2u;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0u) {
                break;
            }
        }
        ++cursor;
    }
    if (depth != 0u) {
        return false;
    }
    *out_begin = value_start + 1u;
    *out_end = cursor;
    return true;
}

bool FindArrayRange(const std::string& text,
                    std::string_view key,
                    std::size_t range_begin,
                    std::size_t range_end,
                    std::size_t* out_begin,
                    std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return false;
    }
    const std::size_t value_start = SkipWs(text, colon + 1u, range_end);
    if (value_start >= range_end || text[value_start] != '[') {
        return false;
    }

    std::size_t depth = 1u;
    std::size_t cursor = value_start + 1u;
    bool in_string = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_string) {
            if (ch == '\\' && cursor + 1u < range_end) {
                cursor += 2u;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0u) {
                break;
            }
        }
        ++cursor;
    }
    if (depth != 0u) {
        return false;
    }
    *out_begin = value_start + 1u;
    *out_end = cursor;
    return true;
}

bool FindBoolField(const std::string& text,
                   std::string_view key,
                   std::size_t range_begin,
                   std::size_t range_end,
                   bool* out_value) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return false;
    }
    const std::size_t value_start = SkipWs(text, colon + 1u, range_end);
    if (value_start + 4u <= range_end &&
        text.compare(value_start, 4u, "true") == 0) {
        *out_value = true;
        return true;
    }
    if (value_start + 5u <= range_end &&
        text.compare(value_start, 5u, "false") == 0) {
        *out_value = false;
        return true;
    }
    throw std::runtime_error("Invalid boolean runtime policy field: " +
                             std::string(key));
}

double ParseNumberToken(const std::string& token,
                        std::string_view field_name) {
    try {
        return std::stod(token);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid numeric runtime policy field: " +
                                 std::string(field_name));
    }
}

std::vector<std::uint32_t> ParseBlockedChannels(
    const std::string& text,
    std::size_t range_begin,
    std::size_t range_end) {
    std::size_t array_begin = 0u;
    std::size_t array_end = 0u;
    if (!FindArrayRange(text, "blocked_channels", range_begin, range_end,
                        &array_begin, &array_end)) {
        return {};
    }

    std::vector<std::uint32_t> blocked;
    std::size_t cursor = array_begin;
    while (cursor < array_end) {
        cursor = SkipWs(text, cursor, array_end);
        if (cursor >= array_end) {
            break;
        }
        if (text[cursor] == ',') {
            ++cursor;
            continue;
        }
        std::size_t value_end = cursor;
        while (value_end < array_end &&
               (std::isdigit(static_cast<unsigned char>(text[value_end])) != 0 ||
                text[value_end] == '+' || text[value_end] == '-')) {
            ++value_end;
        }
        if (value_end == cursor) {
            throw std::runtime_error(
                "control.blocked_channels contains a non-numeric entry");
        }
        const double parsed = ParseNumberToken(
            text.substr(cursor, value_end - cursor), "blocked_channels");
        if (std::isnan(parsed) || parsed < 0.0 ||
            parsed > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(
                "control.blocked_channels contains an out-of-range channel");
        }
        blocked.push_back(static_cast<std::uint32_t>(parsed));
        cursor = value_end;
    }

    std::sort(blocked.begin(), blocked.end());
    blocked.erase(std::unique(blocked.begin(), blocked.end()), blocked.end());
    return blocked;
}

std::filesystem::path ResolveRuntimePolicyPath(const ControlConfig* config) {
    std::filesystem::path path = GetEnvironmentPath(L"SVG_MB_RUNTIME_POLICY");
    if (!path.empty()) {
        return std::filesystem::absolute(path).lexically_normal();
    }
    if (config != nullptr && !config->runtime_policy_path.empty()) {
        return config->runtime_policy_path;
    }
    return {};
}

}  // namespace

RuntimeWritePolicy LoadRuntimeWritePolicy(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::runtime_error("Runtime policy path is empty.");
    }

    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path).lexically_normal();
    const std::string text = ReadEntireFile(absolute_path);

    RuntimeWritePolicy policy;
    policy.present = true;
    policy.source_path = absolute_path;
    policy.writes_enabled = false;
    policy.restore_on_exit = true;

    std::size_t control_begin = 0u;
    std::size_t control_end = 0u;
    if (!FindObjectRange(text, "control", 0u, text.size(),
                         &control_begin, &control_end)) {
        throw std::runtime_error(
            "Runtime policy missing control object: " +
            absolute_path.string());
    }

    bool writes_enabled = false;
    if (FindBoolField(text, "writes_enabled", control_begin, control_end,
                      &writes_enabled)) {
        policy.writes_enabled = writes_enabled;
    }

    bool restore_on_exit = true;
    if (FindBoolField(text, "restore_on_exit", control_begin, control_end,
                      &restore_on_exit)) {
        policy.restore_on_exit = restore_on_exit;
    }

    policy.blocked_channels =
        ParseBlockedChannels(text, control_begin, control_end);
    return policy;
}

RuntimeWritePolicy ResolveRuntimeWritePolicy(const ControlConfig* config) {
    const std::filesystem::path path = ResolveRuntimePolicyPath(config);
    if (path.empty()) {
        return RuntimeWritePolicy{};
    }
    return LoadRuntimeWritePolicy(path);
}

bool RuntimeWritePolicyBlocksChannel(const RuntimeWritePolicy& policy,
                                     std::uint32_t channel) {
    return std::find(policy.blocked_channels.begin(),
                     policy.blocked_channels.end(),
                     channel) != policy.blocked_channels.end();
}

}  // namespace svg_mb_control
