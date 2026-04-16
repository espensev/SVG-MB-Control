#include "runtime_snapshot.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace svg_mb_control {

namespace {

std::size_t SkipWhitespace(const std::string& text,
                           std::size_t offset,
                           std::size_t limit) {
    while (offset < limit &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
        ++offset;
    }
    return offset;
}

std::optional<bool> FindBoolFieldInRange(const std::string& text,
                                         std::size_t range_begin,
                                         std::size_t range_end,
                                         std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::nullopt;
    }
    const std::size_t value_start = SkipWhitespace(text, colon + 1u, range_end);
    if (value_start + 4u <= range_end &&
        text.compare(value_start, 4u, "true") == 0) {
        return true;
    }
    if (value_start + 5u <= range_end &&
        text.compare(value_start, 5u, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<double> FindNumericFieldInRange(const std::string& text,
                                              std::size_t range_begin,
                                              std::size_t range_end,
                                              std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::nullopt;
    }
    std::size_t value_start = SkipWhitespace(text, colon + 1u, range_end);
    std::size_t value_end = value_start;
    while (value_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[value_end])) != 0 ||
            text[value_end] == '.' || text[value_end] == '-' ||
            text[value_end] == '+' || text[value_end] == 'e' ||
            text[value_end] == 'E')) {
        ++value_end;
    }
    if (value_end == value_start) {
        return std::nullopt;
    }
    try {
        return std::stod(text.substr(value_start, value_end - value_start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> FindStringFieldInRange(const std::string& text,
                                                  std::size_t range_begin,
                                                  std::size_t range_end,
                                                  std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::nullopt;
    }
    const std::size_t value_start = SkipWhitespace(text, colon + 1u, range_end);
    if (value_start >= range_end || text[value_start] != '"') {
        return std::nullopt;
    }
    std::string output;
    for (std::size_t index = value_start + 1u; index < range_end; ++index) {
        const char ch = text[index];
        if (ch == '\\') {
            if (index + 1u >= range_end) {
                return std::nullopt;
            }
            const char escaped = text[++index];
            switch (escaped) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return std::nullopt;
            }
            continue;
        }
        if (ch == '"') {
            return output;
        }
        output.push_back(ch);
    }
    return std::nullopt;
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 8u);
    for (char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::array<char, 8> escape{};
                    std::snprintf(escape.data(), escape.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += escape.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

bool FindTopLevelArrayRange(const std::string& text,
                            std::string_view key,
                            std::size_t* out_begin,
                            std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token);
    if (key_offset == std::string::npos) {
        return false;
    }
    const std::size_t bracket_open = text.find('[', key_offset + token.size());
    if (bracket_open == std::string::npos) {
        return false;
    }
    std::size_t depth = 1u;
    std::size_t cursor = bracket_open + 1u;
    bool in_string = false;
    while (cursor < text.size() && depth > 0u) {
        const char ch = text[cursor];
        if (in_string) {
            if (ch == '\\' && cursor + 1u < text.size()) {
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
                *out_begin = bracket_open + 1u;
                *out_end = cursor;
                return true;
            }
        }
        ++cursor;
    }
    throw std::runtime_error("runtime snapshot array not terminated: " +
                             std::string(key));
}

template <class F>
void ForEachObjectInRange(const std::string& text,
                          std::size_t range_begin,
                          std::size_t range_end,
                          F&& visit) {
    std::size_t cursor = range_begin;
    while (cursor < range_end) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= range_end) {
            break;
        }
        std::size_t depth = 1u;
        std::size_t obj_close = obj_open + 1u;
        bool in_string = false;
        while (obj_close < range_end && depth > 0u) {
            const char ch = text[obj_close];
            if (in_string) {
                if (ch == '\\' && obj_close + 1u < range_end) {
                    obj_close += 2u;
                    continue;
                }
                if (ch == '"') {
                    in_string = false;
                }
                ++obj_close;
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
            ++obj_close;
        }
        if (depth != 0u) {
            throw std::runtime_error("runtime snapshot object not terminated");
        }
        visit(obj_open, obj_close);
        cursor = obj_close + 1u;
    }
}

}  // namespace

RuntimeSnapshot ParseRuntimeSnapshotJson(const std::string& snapshot_json) {
    RuntimeSnapshot snapshot;

    if (const auto snapshot_time = FindStringFieldInRange(
            snapshot_json, 0u, snapshot_json.size(), "snapshot_time")) {
        snapshot.snapshot_time_iso = *snapshot_time;
    }
    if (const auto policy = FindBoolFieldInRange(
            snapshot_json, 0u, snapshot_json.size(), "policy_writes_enabled")) {
        snapshot.policy_writes_enabled_present = true;
        snapshot.policy_writes_enabled = *policy;
    }

    std::size_t array_begin = 0u;
    std::size_t array_end = 0u;
    if (FindTopLevelArrayRange(snapshot_json, "amd_sensors",
                               &array_begin, &array_end)) {
        ForEachObjectInRange(
            snapshot_json, array_begin, array_end,
            [&](std::size_t obj_open, std::size_t obj_close) {
                const auto label = FindStringFieldInRange(
                    snapshot_json, obj_open, obj_close, "label");
                const auto temperature = FindNumericFieldInRange(
                    snapshot_json, obj_open, obj_close, "temperature_c");
                if (!label || !temperature || std::isnan(*temperature)) {
                    return;
                }
                RuntimeAmdSensor sensor;
                sensor.label = *label;
                sensor.temperature_c = *temperature;
                snapshot.amd_sensors.push_back(std::move(sensor));
            });
    }

    if (FindTopLevelArrayRange(snapshot_json, "fans",
                               &array_begin, &array_end)) {
        ForEachObjectInRange(
            snapshot_json, array_begin, array_end,
            [&](std::size_t obj_open, std::size_t obj_close) {
                const auto channel = FindNumericFieldInRange(
                    snapshot_json, obj_open, obj_close, "channel");
                if (!channel || std::isnan(*channel)) {
                    return;
                }
                RuntimeFanSnapshot fan;
                fan.channel = static_cast<std::uint32_t>(*channel);
                if (const auto label = FindStringFieldInRange(
                        snapshot_json, obj_open, obj_close, "label")) {
                    fan.label = *label;
                }
                if (const auto rpm = FindNumericFieldInRange(
                        snapshot_json, obj_open, obj_close, "rpm")) {
                    fan.rpm = static_cast<std::uint16_t>(*rpm);
                }
                if (const auto tach_raw = FindNumericFieldInRange(
                        snapshot_json, obj_open, obj_close, "tach_raw")) {
                    fan.tach_raw = static_cast<std::uint16_t>(*tach_raw);
                }
                if (const auto duty = FindNumericFieldInRange(
                        snapshot_json, obj_open, obj_close, "duty_raw")) {
                    fan.duty_raw = static_cast<std::uint8_t>(*duty);
                }
                if (const auto mode = FindNumericFieldInRange(
                        snapshot_json, obj_open, obj_close, "mode_raw")) {
                    fan.mode_raw = static_cast<std::uint8_t>(*mode);
                }
                if (const auto duty_percent = FindNumericFieldInRange(
                        snapshot_json, obj_open, obj_close, "duty_percent")) {
                    fan.duty_percent = *duty_percent;
                }
                if (const auto tach_valid = FindBoolFieldInRange(
                        snapshot_json, obj_open, obj_close, "tach_valid")) {
                    fan.tach_valid = *tach_valid;
                }
                if (const auto manual_override = FindBoolFieldInRange(
                        snapshot_json, obj_open, obj_close, "manual_override")) {
                    fan.manual_override = *manual_override;
                }
                if (const auto write_allowed = FindBoolFieldInRange(
                        snapshot_json, obj_open, obj_close, "write_allowed")) {
                    fan.write_allowed = *write_allowed;
                }
                if (const auto policy_blocked = FindBoolFieldInRange(
                        snapshot_json, obj_open, obj_close, "policy_blocked")) {
                    fan.policy_blocked = *policy_blocked;
                }
                if (const auto effective = FindBoolFieldInRange(
                        snapshot_json, obj_open, obj_close,
                        "effective_write_allowed")) {
                    fan.effective_write_allowed = *effective;
                }
                snapshot.fans.push_back(std::move(fan));
            });
    }

    return snapshot;
}

const RuntimeFanSnapshot* FindRuntimeFanChannel(const RuntimeSnapshot& snapshot,
                                                std::uint32_t channel) {
    for (const auto& fan : snapshot.fans) {
        if (fan.channel == channel) {
            return &fan;
        }
    }
    return nullptr;
}

RuntimeFanSnapshot* FindMutableRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                                 std::uint32_t channel) {
    for (auto& fan : snapshot.fans) {
        if (fan.channel == channel) {
            return &fan;
        }
    }
    return nullptr;
}

RuntimeFanSnapshot& UpsertRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                            std::uint32_t channel) {
    if (RuntimeFanSnapshot* existing =
            FindMutableRuntimeFanChannel(snapshot, channel)) {
        return *existing;
    }
    snapshot.fans.push_back(RuntimeFanSnapshot{});
    snapshot.fans.back().channel = channel;
    return snapshot.fans.back();
}

double FindRuntimeAmdSensorTemperature(const RuntimeSnapshot& snapshot,
                                       const std::string& label) {
    for (const auto& sensor : snapshot.amd_sensors) {
        if (sensor.label == label) {
            return sensor.temperature_c;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::string SerializeRuntimeSnapshotJson(const RuntimeSnapshot& snapshot) {
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"snapshot_time\": \"" << JsonEscape(snapshot.snapshot_time_iso)
           << "\",\n"
           << "  \"policy_writes_enabled_present\": "
           << (snapshot.policy_writes_enabled_present ? "true" : "false")
           << ",\n"
           << "  \"policy_writes_enabled\": "
           << (snapshot.policy_writes_enabled ? "true" : "false") << ",\n"
           << "  \"amd_sensors\": [";
    for (std::size_t index = 0u; index < snapshot.amd_sensors.size(); ++index) {
        const auto& sensor = snapshot.amd_sensors[index];
        if (index > 0u) {
            stream << ",";
        }
        stream << "\n    {\n"
               << "      \"label\": \"" << JsonEscape(sensor.label) << "\",\n"
               << "      \"temperature_c\": " << sensor.temperature_c << "\n"
               << "    }";
    }
    if (!snapshot.amd_sensors.empty()) {
        stream << "\n  ";
    }
    stream << "],\n"
           << "  \"gpu\": {\n"
           << "    \"available\": "
           << (snapshot.gpu.available ? "true" : "false") << ",\n"
           << "    \"core_c\": " << snapshot.gpu.core_c << ",\n"
           << "    \"memjn_c\": " << snapshot.gpu.memjn_c << ",\n"
           << "    \"hotspot_c\": " << snapshot.gpu.hotspot_c << ",\n"
           << "    \"gpu_name\": \"" << JsonEscape(snapshot.gpu.gpu_name)
           << "\",\n"
           << "    \"last_warning\": \""
           << JsonEscape(snapshot.gpu.last_warning) << "\"\n"
           << "  },\n"
           << "  \"fans\": [";
    for (std::size_t index = 0u; index < snapshot.fans.size(); ++index) {
        const auto& fan = snapshot.fans[index];
        if (index > 0u) {
            stream << ",";
        }
        stream << "\n    {\n"
               << "      \"channel\": " << fan.channel << ",\n"
               << "      \"label\": \"" << JsonEscape(fan.label) << "\",\n"
               << "      \"rpm\": " << fan.rpm << ",\n"
               << "      \"tach_raw\": " << fan.tach_raw << ",\n"
               << "      \"duty_raw\": " << static_cast<unsigned int>(fan.duty_raw) << ",\n"
               << "      \"mode_raw\": " << static_cast<unsigned int>(fan.mode_raw) << ",\n"
               << "      \"duty_percent\": " << fan.duty_percent << ",\n"
               << "      \"tach_valid\": "
               << (fan.tach_valid ? "true" : "false") << ",\n"
               << "      \"manual_override\": "
               << (fan.manual_override ? "true" : "false") << ",\n"
               << "      \"write_allowed\": "
               << (fan.write_allowed ? "true" : "false") << ",\n"
               << "      \"policy_blocked\": "
               << (fan.policy_blocked ? "true" : "false") << ",\n"
               << "      \"effective_write_allowed\": "
               << (fan.effective_write_allowed ? "true" : "false") << "\n"
               << "    }";
    }
    if (!snapshot.fans.empty()) {
        stream << "\n  ";
    }
    stream << "]\n}\n";
    return stream.str();
}

bool WriteRuntimeSnapshotJsonFile(const std::filesystem::path& target_path,
                                  const RuntimeSnapshot& snapshot) {
    if (target_path.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = target_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    if (ec) {
        return false;
    }

    const std::filesystem::path target = target_path;
    const std::filesystem::path temp = target.parent_path() /
                                       (target.filename().string() + ".tmp");
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return false;
        }
        stream << SerializeRuntimeSnapshotJson(snapshot);
        stream.flush();
        if (stream.fail()) {
            return false;
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool WriteRuntimeSnapshotFile(const std::filesystem::path& runtime_home,
                              const RuntimeSnapshot& snapshot) {
    return WriteRuntimeSnapshotJsonFile(runtime_home / "current_state.json",
                                        snapshot);
}

}  // namespace svg_mb_control
