#include "pending_writes.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace svg_mb_control {

namespace {

std::string ReadEntireFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open pending-writes sidecar: " +
                                 path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::size_t SkipWhitespace(const std::string& text, std::size_t offset) {
    while (offset < text.size() &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
        ++offset;
    }
    return offset;
}

std::optional<std::string> FindStringField(const std::string& text,
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
    const std::size_t value_start = SkipWhitespace(text, colon + 1u);
    if (value_start >= range_end || text[value_start] != '"') {
        throw std::runtime_error("Expected string for key: " + std::string(key));
    }
    std::string output;
    for (std::size_t index = value_start + 1u; index < range_end; ++index) {
        const char ch = text[index];
        if (ch == '\\') {
            if (index + 1u >= range_end) {
                throw std::runtime_error("Truncated escape in pending-writes.");
            }
            const char escaped = text[++index];
            switch (escaped) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default:
                    throw std::runtime_error("Unsupported escape in pending-writes.");
            }
            continue;
        }
        if (ch == '"') {
            return output;
        }
        output.push_back(ch);
    }
    throw std::runtime_error("Unterminated string in pending-writes.");
}

std::optional<double> FindNumericField(const std::string& text,
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
    std::size_t value_start = SkipWhitespace(text, colon + 1u);
    std::size_t value_end = value_start;
    while (value_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[value_end])) != 0 ||
            text[value_end] == '.' || text[value_end] == '-' ||
            text[value_end] == '+' || text[value_end] == 'e' ||
            text[value_end] == 'E')) {
        ++value_end;
    }
    if (value_end == value_start) {
        throw std::runtime_error("Expected number for key: " + std::string(key));
    }
    try {
        return std::stod(text.substr(value_start, value_end - value_start));
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid number for key: " + std::string(key));
    }
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2u);
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
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += buffer.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

std::string FormatEntries(const std::vector<PendingWriteEntry>& entries) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"entries\": [";
    for (std::size_t index = 0u; index < entries.size(); ++index) {
        const PendingWriteEntry& entry = entries[index];
        if (index > 0u) {
            out << ",";
        }
        out << "\n    {\n"
            << "      \"channel\": " << entry.channel << ",\n"
            << "      \"baseline_duty_raw\": "
            << static_cast<unsigned>(entry.baseline_duty_raw) << ",\n"
            << "      \"baseline_mode_raw\": "
            << static_cast<unsigned>(entry.baseline_mode_raw) << ",\n"
            << "      \"target_pct\": " << entry.target_pct << ",\n"
            << "      \"requested_hold_ms\": " << entry.requested_hold_ms << ",\n"
            << "      \"started_iso\": \""
            << JsonEscape(entry.started_iso) << "\",\n"
            << "      \"child_pid\": " << entry.child_pid << "\n"
            << "    }";
    }
    if (!entries.empty()) {
        out << "\n  ";
    }
    out << "]\n}\n";
    return out.str();
}

}  // namespace

std::filesystem::path PendingWritesSidecarPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "pending_writes.json";
}

std::vector<PendingWriteEntry> ReadPendingWrites(
    const std::filesystem::path& runtime_home) {
    const std::filesystem::path path = PendingWritesSidecarPath(runtime_home);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }

    const std::string text = ReadEntireFile(path);
    std::vector<PendingWriteEntry> result;

    // Find the entries array.
    const std::size_t entries_key = text.find("\"entries\"");
    if (entries_key == std::string::npos) {
        return {};
    }
    const std::size_t array_open = text.find('[', entries_key);
    if (array_open == std::string::npos) {
        throw std::runtime_error("pending-writes sidecar missing entries array.");
    }
    // Find matching closing bracket (naive: counts braces inside).
    std::size_t depth = 1u;
    std::size_t array_close = array_open + 1u;
    bool in_string = false;
    while (array_close < text.size() && depth > 0u) {
        const char ch = text[array_close];
        if (in_string) {
            if (ch == '\\' && array_close + 1u < text.size()) {
                array_close += 2u;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            ++array_close;
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
        ++array_close;
    }
    if (depth != 0u) {
        throw std::runtime_error("pending-writes sidecar: unterminated entries array.");
    }

    // Scan for object opens inside the array.
    std::size_t cursor = array_open + 1u;
    while (cursor < array_close) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= array_close) {
            break;
        }
        // Find matching closing brace.
        std::size_t obj_depth = 1u;
        std::size_t obj_close = obj_open + 1u;
        bool obj_in_string = false;
        while (obj_close < array_close && obj_depth > 0u) {
            const char ch = text[obj_close];
            if (obj_in_string) {
                if (ch == '\\' && obj_close + 1u < array_close) {
                    obj_close += 2u;
                    continue;
                }
                if (ch == '"') {
                    obj_in_string = false;
                }
                ++obj_close;
                continue;
            }
            if (ch == '"') {
                obj_in_string = true;
            } else if (ch == '{') {
                ++obj_depth;
            } else if (ch == '}') {
                --obj_depth;
                if (obj_depth == 0u) {
                    break;
                }
            }
            ++obj_close;
        }
        if (obj_depth != 0u) {
            throw std::runtime_error("pending-writes sidecar: unterminated entry.");
        }

        PendingWriteEntry entry;
        const auto channel = FindNumericField(text, obj_open, obj_close, "channel");
        const auto duty = FindNumericField(text, obj_open, obj_close, "baseline_duty_raw");
        const auto mode = FindNumericField(text, obj_open, obj_close, "baseline_mode_raw");
        const auto target = FindNumericField(text, obj_open, obj_close, "target_pct");
        const auto hold = FindNumericField(text, obj_open, obj_close, "requested_hold_ms");
        const auto pid = FindNumericField(text, obj_open, obj_close, "child_pid");
        const auto iso = FindStringField(text, obj_open, obj_close, "started_iso");
        if (!channel || !duty || !mode || !target || !hold) {
            throw std::runtime_error("pending-writes entry missing required field.");
        }
        entry.channel = static_cast<std::uint32_t>(*channel);
        entry.baseline_duty_raw = static_cast<std::uint8_t>(*duty);
        entry.baseline_mode_raw = static_cast<std::uint8_t>(*mode);
        entry.target_pct = *target;
        entry.requested_hold_ms = static_cast<std::uint32_t>(*hold);
        entry.child_pid = pid ? static_cast<std::uint32_t>(*pid) : 0u;
        entry.started_iso = iso ? *iso : std::string();
        result.push_back(std::move(entry));

        cursor = obj_close + 1u;
    }

    return result;
}

void WritePendingWrites(const std::filesystem::path& runtime_home,
                        const std::vector<PendingWriteEntry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) {
        throw std::runtime_error("Could not create runtime home: " + ec.message());
    }

    const std::filesystem::path target = PendingWritesSidecarPath(runtime_home);
    const std::filesystem::path temp =
        runtime_home / "pending_writes.json.tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            throw std::runtime_error("Could not open pending-writes temp file.");
        }
        stream << FormatEntries(entries);
        stream.flush();
        if (stream.fail()) {
            throw std::runtime_error("Failed writing pending-writes temp file.");
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("Failed to rename pending-writes file: " +
                                 ec.message());
    }
}

void UpsertPendingWrite(const std::filesystem::path& runtime_home,
                        const PendingWriteEntry& entry) {
    std::vector<PendingWriteEntry> entries = ReadPendingWrites(runtime_home);
    for (auto& existing : entries) {
        if (existing.channel == entry.channel) {
            existing = entry;
            WritePendingWrites(runtime_home, entries);
            return;
        }
    }
    entries.push_back(entry);
    WritePendingWrites(runtime_home, entries);
}

void RemovePendingWrite(const std::filesystem::path& runtime_home,
                        std::uint32_t channel) {
    std::vector<PendingWriteEntry> entries = ReadPendingWrites(runtime_home);
    std::vector<PendingWriteEntry> remaining;
    remaining.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.channel != channel) {
            remaining.push_back(entry);
        }
    }
    if (remaining.size() == entries.size()) {
        return;
    }
    WritePendingWrites(runtime_home, remaining);
}

}  // namespace svg_mb_control
