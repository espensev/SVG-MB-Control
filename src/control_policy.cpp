#include "control_policy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace svg_mb_control {

TempBlend ParseTempBlend(const std::string& text) {
    if (text == "cpu_only" || text == "cpu") return TempBlend::CpuOnly;
    if (text == "gpu_only" || text == "gpu") return TempBlend::GpuOnly;
    if (text == "max_cpu_gpu" || text == "max") return TempBlend::MaxCpuGpu;
    throw std::runtime_error("Unknown temp blend: " + text);
}

std::string TempBlendToString(TempBlend blend) {
    switch (blend) {
        case TempBlend::CpuOnly: return "cpu_only";
        case TempBlend::GpuOnly: return "gpu_only";
        case TempBlend::MaxCpuGpu: return "max_cpu_gpu";
    }
    return "cpu_only";
}

double BlendTemps(const TempInputs& inputs, TempBlend mode) {
    constexpr double kAbsoluteZeroC = -273.15;
    const double cpu = inputs.cpu_available ? inputs.cpu_c : kAbsoluteZeroC;
    const double gpu = inputs.gpu_available ? inputs.gpu_c : kAbsoluteZeroC;
    switch (mode) {
        case TempBlend::CpuOnly: return cpu;
        case TempBlend::GpuOnly: return gpu;
        case TempBlend::MaxCpuGpu: return (std::max)(cpu, gpu);
    }
    return cpu;
}

double LookupCurve(const std::vector<CurvePoint>& curve,
                   double temp_c,
                   double min_floor_pct) {
    if (curve.empty()) {
        return (std::max)(0.0, min_floor_pct);
    }
    double raw = 0.0;
    if (temp_c <= curve.front().temp_c) {
        raw = curve.front().duty_pct;
    } else if (temp_c >= curve.back().temp_c) {
        raw = curve.back().duty_pct;
    } else {
        for (std::size_t index = 1u; index < curve.size(); ++index) {
            const CurvePoint& lo = curve[index - 1u];
            const CurvePoint& hi = curve[index];
            if (temp_c <= hi.temp_c) {
                const double span = hi.temp_c - lo.temp_c;
                if (span <= 0.0) {
                    raw = hi.duty_pct;
                } else {
                    const double t = (temp_c - lo.temp_c) / span;
                    raw = lo.duty_pct + t * (hi.duty_pct - lo.duty_pct);
                }
                break;
            }
        }
    }
    const double floor = (std::max)(0.0, min_floor_pct);
    return std::clamp(raw, floor, 100.0);
}

namespace {

std::size_t SkipWhitespaceC(const std::string& text, std::size_t offset,
                            std::size_t limit) {
    while (offset < limit &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
        ++offset;
    }
    return offset;
}

// Find the [..] range that is the value of the top-level `amd_sensors`
// field in the snapshot JSON.
bool FindAmdSensorsArrayRange(const std::string& text,
                              std::size_t* out_begin,
                              std::size_t* out_end) {
    const std::string token = "\"amd_sensors\"";
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
            if (ch == '"') in_string = false;
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
    return false;
}

// Scan object braces inside range. For each {...} object, invoke visit
// with (obj_begin, obj_end) where obj_end is the position of the closing
// brace.
template <class F>
void ForEachObject(const std::string& text, std::size_t range_begin,
                   std::size_t range_end, F&& visit) {
    std::size_t cursor = range_begin;
    while (cursor < range_end) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= range_end) break;
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
                if (ch == '"') in_string = false;
                ++obj_close;
                continue;
            }
            if (ch == '"') {
                in_string = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0u) break;
            }
            ++obj_close;
        }
        if (depth != 0u) break;
        visit(obj_open, obj_close);
        cursor = obj_close + 1u;
    }
}

std::string FindObjectStringField(const std::string& text,
                                  std::size_t range_begin,
                                  std::size_t range_end,
                                  std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return {};
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return {};
    const std::size_t value_start =
        SkipWhitespaceC(text, colon + 1u, range_end);
    if (value_start >= range_end || text[value_start] != '"') return {};
    std::string output;
    for (std::size_t index = value_start + 1u; index < range_end; ++index) {
        const char ch = text[index];
        if (ch == '\\') {
            if (index + 1u >= range_end) return {};
            const char escaped = text[++index];
            switch (escaped) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return {};
            }
            continue;
        }
        if (ch == '"') return output;
        output.push_back(ch);
    }
    return {};
}

double FindObjectNumericField(const std::string& text,
                              std::size_t range_begin,
                              std::size_t range_end,
                              std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::size_t value_start = SkipWhitespaceC(text, colon + 1u, range_end);
    std::size_t value_end = value_start;
    while (value_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[value_end])) != 0 ||
            text[value_end] == '.' || text[value_end] == '-' ||
            text[value_end] == '+' || text[value_end] == 'e' ||
            text[value_end] == 'E')) {
        ++value_end;
    }
    if (value_end == value_start) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    try {
        return std::stod(text.substr(value_start, value_end - value_start));
    } catch (const std::exception&) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

}  // namespace

namespace {

bool FindFansArrayRange(const std::string& text,
                        std::size_t* out_begin,
                        std::size_t* out_end) {
    const std::string token = "\"fans\"";
    const std::size_t key_offset = text.find(token);
    if (key_offset == std::string::npos) return false;
    const std::size_t bracket_open = text.find('[', key_offset + token.size());
    if (bracket_open == std::string::npos) return false;
    std::size_t depth = 1u;
    std::size_t cursor = bracket_open + 1u;
    bool in_string = false;
    while (cursor < text.size() && depth > 0u) {
        const char ch = text[cursor];
        if (in_string) {
            if (ch == '\\' && cursor + 1u < text.size()) { cursor += 2u; continue; }
            if (ch == '"') in_string = false;
            ++cursor;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '[') ++depth;
        else if (ch == ']') {
            --depth;
            if (depth == 0u) {
                *out_begin = bracket_open + 1u;
                *out_end = cursor;
                return true;
            }
        }
        ++cursor;
    }
    return false;
}

}  // namespace

FanRawState ExtractFanRawState(const std::string& snapshot_json,
                               std::uint32_t channel) {
    FanRawState result;
    std::size_t arr_begin = 0u;
    std::size_t arr_end = 0u;
    if (!FindFansArrayRange(snapshot_json, &arr_begin, &arr_end)) {
        return result;
    }
    ForEachObject(
        snapshot_json, arr_begin, arr_end,
        [&](std::size_t obj_open, std::size_t obj_close) {
            if (result.present) return;
            const double ch = FindObjectNumericField(
                snapshot_json, obj_open, obj_close, "channel");
            if (std::isnan(ch) ||
                static_cast<std::uint32_t>(ch) != channel) {
                return;
            }
            const double duty = FindObjectNumericField(
                snapshot_json, obj_open, obj_close, "duty_raw");
            const double mode = FindObjectNumericField(
                snapshot_json, obj_open, obj_close, "mode_raw");
            if (std::isnan(duty) || std::isnan(mode)) return;
            result.present = true;
            result.duty_raw = static_cast<std::uint8_t>(duty);
            result.mode_raw = static_cast<std::uint8_t>(mode);
        });
    return result;
}

double ExtractAmdSensorTemperature(const std::string& snapshot_json,
                                   const std::string& label) {
    std::size_t arr_begin = 0u;
    std::size_t arr_end = 0u;
    if (!FindAmdSensorsArrayRange(snapshot_json, &arr_begin, &arr_end)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double result = std::numeric_limits<double>::quiet_NaN();
    ForEachObject(
        snapshot_json, arr_begin, arr_end,
        [&](std::size_t obj_open, std::size_t obj_close) {
            if (!std::isnan(result)) return;
            const std::string object_label = FindObjectStringField(
                snapshot_json, obj_open, obj_close, "label");
            if (object_label != label) return;
            result = FindObjectNumericField(
                snapshot_json, obj_open, obj_close, "temperature_c");
        });
    return result;
}

}  // namespace svg_mb_control
