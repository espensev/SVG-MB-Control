#include "control_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace svg_mb_control {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open control config: " + path.string());
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

std::size_t FindKeyStart(const std::string& text, std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    return text.find(token);
}

std::size_t FindValueStart(const std::string& text, std::string_view key) {
    const std::size_t key_start = FindKeyStart(text, key);
    if (key_start == std::string::npos) {
        return std::string::npos;
    }

    const std::size_t colon = text.find(':', key_start + key.size() + 2u);
    if (colon == std::string::npos) {
        throw std::runtime_error("Malformed control config near key: " + std::string(key));
    }
    return SkipWhitespace(text, colon + 1u);
}

std::string ParseJsonStringLiteral(const std::string& text,
                                   std::size_t offset,
                                   std::string_view key) {
    if (offset >= text.size() || text[offset] != '"') {
        throw std::runtime_error("Expected string value for key: " + std::string(key));
    }

    std::string output;
    for (std::size_t index = offset + 1u; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\\') {
            if (index + 1u >= text.size()) {
                throw std::runtime_error("Invalid escape sequence in control config.");
            }
            const char escaped = text[++index];
            switch (escaped) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default:
                    throw std::runtime_error("Unsupported escape sequence in control config.");
            }
            continue;
        }
        if (ch == '"') {
            return output;
        }
        output.push_back(ch);
    }

    throw std::runtime_error("Unterminated string value in control config.");
}

std::optional<std::string> ParseOptionalStringField(const std::string& text,
                                                    std::string_view key) {
    const std::size_t offset = FindValueStart(text, key);
    if (offset == std::string::npos) {
        return std::nullopt;
    }
    return ParseJsonStringLiteral(text, offset, key);
}

std::optional<std::uint32_t> ParseOptionalUIntField(const std::string& text,
                                                    std::string_view key) {
    const std::size_t offset = FindValueStart(text, key);
    if (offset == std::string::npos) {
        return std::nullopt;
    }
    if (offset >= text.size() || !std::isdigit(static_cast<unsigned char>(text[offset]))) {
        throw std::runtime_error("Expected unsigned integer value for key: " + std::string(key));
    }

    std::size_t end = offset;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
        ++end;
    }

    const std::string digits = text.substr(offset, end - offset);
    const unsigned long value = std::stoul(digits);
    return static_cast<std::uint32_t>(value);
}

std::filesystem::path ResolveConfigRelativePath(const std::filesystem::path& config_path,
                                                const std::string& raw_value) {
    std::filesystem::path path(raw_value);
    if (path.empty()) {
        return {};
    }
    if (path.is_relative()) {
        path = config_path.parent_path() / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

}  // namespace

std::filesystem::path GetEnvironmentPath(std::wstring_view name) {
    std::array<wchar_t, 4096> buffer{};
    const DWORD length = GetEnvironmentVariableW(std::wstring(name).c_str(),
                                                 buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data(), buffer.data() + length);
}

std::filesystem::path ResolveDefaultControlConfigPath() {
    const std::filesystem::path current_exe_dir = []() {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return std::filesystem::path();
        }
        return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
    }();

    const std::vector<std::filesystem::path> candidates = {
        current_exe_dir / "config" / "control.json",
        current_exe_dir / "control.json",
        std::filesystem::current_path() / "config" / "control.json",
        std::filesystem::current_path() / "control.json",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() &&
            std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    return {};
}

ControlConfig LoadControlConfig(const std::filesystem::path& path) {
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path).lexically_normal();
    if (!std::filesystem::exists(absolute_path)) {
        throw std::runtime_error("Control config not found: " + absolute_path.string());
    }

    const std::string text = ReadTextFile(absolute_path);
    ControlConfig config;
    config.source_path = absolute_path;

    if (const auto bench_exe = ParseOptionalStringField(text, "bench_exe_path")) {
        config.bench_exe_path = ResolveConfigRelativePath(absolute_path, *bench_exe);
    }
    if (const auto snapshot_path = ParseOptionalStringField(text, "snapshot_path")) {
        config.snapshot_path = ResolveConfigRelativePath(absolute_path, *snapshot_path);
    }
    if (const auto poll_ms = ParseOptionalUIntField(text, "poll_ms")) {
        config.poll_ms = *poll_ms;
    }

    return config;
}

}  // namespace svg_mb_control
