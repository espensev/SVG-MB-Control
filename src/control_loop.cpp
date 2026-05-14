#include "control_loop.h"

#include "amd_reader.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "pending_writes.h"
#include "runtime_logging.h"
#include "runtime_snapshot.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace svg_mb_control {

namespace {

// ---------- Minimal JSON scanning helpers (local to this TU) ----------

std::size_t SkipWs(const std::string& t, std::size_t o, std::size_t limit) {
    while (o < limit &&
           std::isspace(static_cast<unsigned char>(t[o])) != 0) {
        ++o;
    }
    return o;
}

bool FindObjectRange(const std::string& text, std::string_view key,
                     std::size_t range_begin, std::size_t range_end,
                     std::size_t* out_begin, std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return false;
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '{') return false;
    std::size_t depth = 1u;
    std::size_t cursor = val_start + 1u;
    bool in_str = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_str) {
            if (ch == '\\' && cursor + 1u < range_end) { cursor += 2u; continue; }
            if (ch == '"') in_str = false;
            ++cursor;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '{') ++depth;
        else if (ch == '}') { --depth; if (depth == 0u) break; }
        ++cursor;
    }
    if (depth != 0u) return false;
    *out_begin = val_start + 1u;
    *out_end = cursor;
    return true;
}

bool FindArrayRange(const std::string& text, std::string_view key,
                    std::size_t range_begin, std::size_t range_end,
                    std::size_t* out_begin, std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return false;
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '[') return false;
    std::size_t depth = 1u;
    std::size_t cursor = val_start + 1u;
    bool in_str = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_str) {
            if (ch == '\\' && cursor + 1u < range_end) { cursor += 2u; continue; }
            if (ch == '"') in_str = false;
            ++cursor;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '[') ++depth;
        else if (ch == ']') { --depth; if (depth == 0u) break; }
        ++cursor;
    }
    if (depth != 0u) return false;
    *out_begin = val_start + 1u;
    *out_end = cursor;
    return true;
}

template <class F>
void ForEachObjectInRange(const std::string& text,
                          std::size_t range_begin, std::size_t range_end,
                          F&& visit) {
    std::size_t cursor = range_begin;
    while (cursor < range_end) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= range_end) break;
        std::size_t depth = 1u;
        std::size_t obj_close = obj_open + 1u;
        bool in_str = false;
        while (obj_close < range_end && depth > 0u) {
            const char ch = text[obj_close];
            if (in_str) {
                if (ch == '\\' && obj_close + 1u < range_end) { obj_close += 2u; continue; }
                if (ch == '"') in_str = false;
                ++obj_close;
                continue;
            }
            if (ch == '"') in_str = true;
            else if (ch == '{') ++depth;
            else if (ch == '}') { --depth; if (depth == 0u) break; }
            ++obj_close;
        }
        if (depth != 0u) break;
        visit(obj_open, obj_close);
        cursor = obj_close + 1u;
    }
}

double FindNumericInRange(const std::string& text, std::string_view key,
                          std::size_t range_begin, std::size_t range_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    std::size_t val_end = val_start;
    while (val_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[val_end])) != 0 ||
            text[val_end] == '.' || text[val_end] == '-' ||
            text[val_end] == '+' || text[val_end] == 'e' ||
            text[val_end] == 'E')) {
        ++val_end;
    }
    if (val_end == val_start) return std::numeric_limits<double>::quiet_NaN();
    try {
        return std::stod(text.substr(val_start, val_end - val_start));
    } catch (const std::exception&) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::string FindStringInRange(const std::string& text, std::string_view key,
                              std::size_t range_begin, std::size_t range_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) return {};
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return {};
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '"') return {};
    std::string output;
    for (std::size_t i = val_start + 1u; i < range_end; ++i) {
        const char ch = text[i];
        if (ch == '\\') {
            if (i + 1u >= range_end) return {};
            const char e = text[++i];
            switch (e) {
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

// ---------- Configuration Validation ----------

void ValidatePercentage(double value, const std::string& field_name,
                       bool allow_nan = false) {
    if (allow_nan && std::isnan(value)) {
        return;
    }
    if (std::isnan(value)) {
        throw std::runtime_error(field_name + " must not be NaN");
    }
    if (value < 0.0 || value > 100.0) {
        throw std::runtime_error(field_name + " must be in [0, 100], got " +
                                std::to_string(value));
    }
}

void ValidatePositive(double value, const std::string& field_name,
                     bool allow_nan = false) {
    if (allow_nan && std::isnan(value)) {
        return;
    }
    if (std::isnan(value)) {
        throw std::runtime_error(field_name + " must not be NaN");
    }
    if (value < 0.0) {
        throw std::runtime_error(field_name + " must be non-negative, got " +
                                std::to_string(value));
    }
}

void ValidateAlpha(double value, const std::string& field_name,
                  bool allow_nan = true) {
    if (allow_nan && std::isnan(value)) {
        return;
    }
    if (std::isnan(value)) {
        throw std::runtime_error(field_name + " must not be NaN");
    }
    if (value < 0.0 || value > 1.0) {
        throw std::runtime_error(field_name + " must be in [0, 1], got " +
                                std::to_string(value));
    }
}

void ValidateCurve(const std::vector<CurvePoint>& curve,
                  const std::string& curve_name) {
    if (curve.empty()) {
        return; // Empty curves are allowed
    }
    for (const auto& point : curve) {
        if (std::isnan(point.temp_c)) {
            throw std::runtime_error(curve_name + " has NaN temperature");
        }
        if (std::isnan(point.duty_pct)) {
            throw std::runtime_error(curve_name + " has NaN duty percentage");
        }
        ValidatePercentage(point.duty_pct,
                         curve_name + " duty_pct at " +
                         std::to_string(point.temp_c) + "C");
    }
}

void ValidateChannelConfig(const ChannelControlConfig& ch,
                          std::uint32_t index) {
    const std::string prefix = "Channel " + std::to_string(ch.channel) +
                              " (index " + std::to_string(index) + ")";

    ValidatePercentage(ch.min_duty_pct, prefix + " min_duty_pct");
    ValidatePercentage(ch.deadband_pct, prefix + " deadband_pct", true);

    ValidatePositive(ch.rise_rate_pct_per_min,
                    prefix + " rise_rate_pct_per_min", true);
    ValidatePositive(ch.fall_rate_pct_per_min,
                    prefix + " fall_rate_pct_per_min", true);

    ValidateAlpha(ch.demand_smoothing_rise_alpha,
                 prefix + " demand_smoothing_rise_alpha");
    ValidateAlpha(ch.demand_smoothing_fall_alpha,
                 prefix + " demand_smoothing_fall_alpha");

    ValidatePercentage(ch.decay_latch_above_pct,
                      prefix + " decay_latch_above_pct", true);
    ValidatePositive(ch.decay_latch_pct_per_min,
                    prefix + " decay_latch_pct_per_min", true);

    // Validate thermal pressure parameters
    if (!std::isnan(ch.thermal_pressure_start_c) ||
        !std::isnan(ch.thermal_pressure_full_c) ||
        !std::isnan(ch.thermal_pressure_rise_pct_per_sec) ||
        !std::isnan(ch.thermal_pressure_fall_pct_per_sec) ||
        !std::isnan(ch.thermal_pressure_max_boost_pct)) {

        // If any thermal pressure param is set, validate the complete set
        if (std::isnan(ch.thermal_pressure_start_c) ||
            std::isnan(ch.thermal_pressure_full_c)) {
            throw std::runtime_error(
                prefix + " thermal_pressure requires both start_c and full_c");
        }

        ValidatePositive(ch.thermal_pressure_rise_pct_per_sec,
                        prefix + " thermal_pressure_rise_pct_per_sec", true);
        ValidatePositive(ch.thermal_pressure_fall_pct_per_sec,
                        prefix + " thermal_pressure_fall_pct_per_sec", true);
        ValidatePercentage(ch.thermal_pressure_max_boost_pct,
                          prefix + " thermal_pressure_max_boost_pct", true);

        if (ch.thermal_pressure_full_c <= ch.thermal_pressure_start_c) {
            throw std::runtime_error(
                prefix + " thermal_pressure_full_c must be > start_c");
        }
    }

    ValidateCurve(ch.curve, prefix + " curve");
    ValidateCurve(ch.cpu_override_curve, prefix + " cpu_override_curve");
}

void ValidateControlLoopConfig(const ControlLoopConfig& cfg,
                              const std::filesystem::path& config_path) {
    if (cfg.poll_tick_ms == 0u) {
        throw std::runtime_error("poll_tick_ms must be > 0");
    }
    if (cfg.poll_tick_ms < 50u) {
        // Warn but don't fail for very fast polling
        std::fprintf(stderr,
                    "Warning: poll_tick_ms=%u is very fast, may cause high CPU usage\n",
                    cfg.poll_tick_ms);
    }

    ValidatePercentage(cfg.deadband_pct, "deadband_pct");

    if (cfg.channels.empty()) {
        throw std::runtime_error(
            "control_loop config has empty channels array: " +
            config_path.string());
    }

    for (std::size_t i = 0; i < cfg.channels.size(); ++i) {
        ValidateChannelConfig(cfg.channels[i], static_cast<std::uint32_t>(i));
    }
}

std::string ReadEntireFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open control config: " +
                                 path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void ParseCurveArray(const std::string& text,
                     std::string_view key,
                     std::size_t range_begin,
                     std::size_t range_end,
                     std::vector<CurvePoint>* curve) {
    std::size_t curve_begin = 0u;
    std::size_t curve_end = 0u;
    if (!FindArrayRange(text, key, range_begin, range_end,
                        &curve_begin, &curve_end)) {
        return;
    }

    ForEachObjectInRange(text, curve_begin, curve_end,
        [&](std::size_t p_open, std::size_t p_close) {
            const double t = FindNumericInRange(text, "temp_c", p_open, p_close);
            const double d = FindNumericInRange(text, "duty_pct", p_open, p_close);
            if (std::isnan(t) || std::isnan(d)) return;
            curve->push_back({t, d});
        });
    std::sort(curve->begin(), curve->end(),
              [](const CurvePoint& a, const CurvePoint& b) {
                  return a.temp_c < b.temp_c;
              });
}

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) return {};
    std::array<char, 32> buf{};
    const std::size_t n = std::strftime(buf.data(), buf.size(),
                                        "%Y-%m-%dT%H:%M:%S", &local);
    return n > 0 ? std::string(buf.data(), n) : std::string();
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2u);
    for (char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::array<char, 8> esc{};
                    std::snprintf(esc.data(), esc.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += esc.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

std::uint32_t EffectiveWriteCooldownMs(const ControlLoopConfig& loop,
                                       const ChannelControlConfig& channel) {
    return channel.write_cooldown_ms > 0u
        ? channel.write_cooldown_ms
        : loop.write_cooldown_ms;
}

double EffectiveDeadbandPct(const ControlLoopConfig& loop,
                            const ChannelControlConfig& channel) {
    return std::isnan(channel.deadband_pct)
        ? loop.deadband_pct
        : channel.deadband_pct;
}

std::uint32_t EffectiveControlHoldMs(const ControlLoopConfig& loop,
                                     const ChannelControlConfig& channel) {
    return channel.control_hold_ms > 0u
        ? channel.control_hold_ms
        : loop.control_hold_ms;
}

double RateLimitSetpoint(double desired_pct,
                         double last_pct,
                         std::uint64_t elapsed_ms,
                         double rise_rate_pct_per_min,
                         double fall_rate_pct_per_min) {
    if (std::isnan(last_pct)) {
        return desired_pct;
    }

    const double delta = desired_pct - last_pct;
    if (std::abs(delta) <= 0.0001) {
        return desired_pct;
    }

    const double rate = delta > 0.0
        ? rise_rate_pct_per_min
        : fall_rate_pct_per_min;
    if (std::isnan(rate) || rate <= 0.0) {
        return desired_pct;
    }

    const double allowed = rate *
        (static_cast<double>(elapsed_ms) / 60000.0);
    if (std::abs(delta) <= allowed) {
        return desired_pct;
    }
    return last_pct + (delta > 0.0 ? allowed : -allowed);
}

double ApplyDemandSmoothing(double raw_desired_pct,
                            double last_smoothed_pct,
                            std::uint64_t elapsed_ms,
                            const ChannelControlConfig& config) {
    if (std::isnan(last_smoothed_pct)) {
        return raw_desired_pct;
    }

    const double delta = raw_desired_pct - last_smoothed_pct;
    if (std::abs(delta) <= 0.0001) {
        return raw_desired_pct;
    }

    if (delta > 0.0) {
        if (std::isnan(config.demand_smoothing_rise_alpha)) {
            return raw_desired_pct;
        }
        const double alpha =
            std::clamp(config.demand_smoothing_rise_alpha, 0.0, 1.0);
        return std::clamp(
            last_smoothed_pct + alpha * delta,
            0.0, 100.0);
    }

    double smoothed = raw_desired_pct;
    if (!std::isnan(config.demand_smoothing_fall_alpha)) {
        const double alpha =
            std::clamp(config.demand_smoothing_fall_alpha, 0.0, 1.0);
        smoothed = last_smoothed_pct + alpha * delta;
    }

    if (!std::isnan(config.decay_latch_pct_per_min) &&
        config.decay_latch_pct_per_min > 0.0 &&
        elapsed_ms > 0u) {
        const bool latch_zone =
            std::isnan(config.decay_latch_above_pct) ||
            last_smoothed_pct >= config.decay_latch_above_pct ||
            smoothed >= config.decay_latch_above_pct;
        if (latch_zone) {
            const double max_drop =
                config.decay_latch_pct_per_min *
                (static_cast<double>(elapsed_ms) / 60000.0);
            smoothed = (std::max)(smoothed, last_smoothed_pct - max_drop);
        }
    }

    return std::clamp(smoothed, 0.0, 100.0);
}

double UpdateThermalPressureBoost(double observed_temp_c,
                                   double current_boost_pct,
                                   std::uint64_t elapsed_ms,
                                   const ChannelControlConfig& config) {
    if (std::isnan(config.thermal_pressure_start_c) ||
        std::isnan(config.thermal_pressure_full_c) ||
        std::isnan(config.thermal_pressure_rise_pct_per_sec) ||
        std::isnan(config.thermal_pressure_fall_pct_per_sec) ||
        std::isnan(config.thermal_pressure_max_boost_pct) ||
        config.thermal_pressure_rise_pct_per_sec <= 0.0 ||
        config.thermal_pressure_fall_pct_per_sec < 0.0 ||
        config.thermal_pressure_max_boost_pct <= 0.0) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(
              current_boost_pct, 0.0, config.thermal_pressure_max_boost_pct);
    if (elapsed_ms == 0u || std::isnan(observed_temp_c)) {
        return boost;
    }

    const double dt_seconds = static_cast<double>(elapsed_ms) / 1000.0;
    if (observed_temp_c >= config.thermal_pressure_start_c) {
        double pressure_scale = 1.0;
        if (config.thermal_pressure_full_c >
            config.thermal_pressure_start_c) {
            // Calculate normalized position in [0, 1]
            double t = std::clamp(
                (observed_temp_c - config.thermal_pressure_start_c) /
                    (config.thermal_pressure_full_c -
                     config.thermal_pressure_start_c),
                0.0, 1.0);

            // Apply smootherstep for smoother ramp (Horner's form for numerical stability)
            pressure_scale = t * t * t * ((6.0 * t - 15.0) * t + 10.0);
        }

        // Anti-windup: only accumulate if below max OR temperature is falling
        const bool below_max = boost < config.thermal_pressure_max_boost_pct;
        const bool temp_falling = observed_temp_c < config.thermal_pressure_start_c;

        if (below_max || temp_falling) {
            boost += config.thermal_pressure_rise_pct_per_sec *
                     pressure_scale * dt_seconds;
        }
    } else {
        boost -= config.thermal_pressure_fall_pct_per_sec * dt_seconds;
    }

    return std::clamp(
        boost, 0.0, config.thermal_pressure_max_boost_pct);
}

}  // namespace

// ------------------------ Config loader --------------------------------

ControlLoopConfig LoadControlLoopConfig(
    const std::filesystem::path& config_path) {

    // Load and parse JSON using nlohmann/json library
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        throw std::runtime_error("Could not open control config: " +
                                config_path.string());
    }

    nlohmann::json root;
    try {
        config_file >> root;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + config_path.string() +
                                ": " + e.what());
    }

    if (!root.contains("control_loop")) {
        throw std::runtime_error(
            "control config missing control_loop object: " + config_path.string());
    }

    const auto& loop_json = root["control_loop"];
    ControlLoopConfig cfg;

    // Parse top-level control loop settings with defaults
    cfg.poll_tick_ms = loop_json.value("poll_tick_ms", cfg.poll_tick_ms);
    cfg.write_cooldown_ms = loop_json.value("write_cooldown_ms", cfg.write_cooldown_ms);
    cfg.deadband_pct = loop_json.value("deadband_pct", cfg.deadband_pct);
    cfg.control_hold_ms = loop_json.value("control_hold_ms", cfg.control_hold_ms);
    cfg.cpu_temp_label = loop_json.value("cpu_temp_label", cfg.cpu_temp_label);

    // Parse channels array
    if (!loop_json.contains("channels") || !loop_json["channels"].is_array()) {
        throw std::runtime_error(
            "control_loop config missing channels array: " + config_path.string());
    }

    const auto& channels_json = loop_json["channels"];
    if (channels_json.empty()) {
        throw std::runtime_error(
            "control_loop config has empty channels array: " + config_path.string());
    }

    for (const auto& ch_json : channels_json) {
        if (!ch_json.contains("channel")) {
            continue; // Skip channels without channel number
        }

        ChannelControlConfig channel;
        channel.channel = ch_json["channel"].get<std::uint32_t>();

        // Optional fields with defaults
        channel.min_duty_pct = ch_json.value("min_duty_pct", channel.min_duty_pct);
        channel.write_cooldown_ms = ch_json.value("write_cooldown_ms",
                                                   channel.write_cooldown_ms);

        if (ch_json.contains("deadband_pct")) {
            channel.deadband_pct = ch_json["deadband_pct"].get<double>();
        }

        channel.control_hold_ms = ch_json.value("control_hold_ms",
                                                channel.control_hold_ms);

        // Parse curve shape
        if (ch_json.contains("curve_shape")) {
            try {
                channel.curve_shape = ParseCurveShape(
                    ch_json["curve_shape"].get<std::string>());
            } catch (const std::exception&) {
                channel.curve_shape = CurveShape::Linear;
            }
        }

        // Parse rate limiting
        if (ch_json.contains("rise_rate_pct_per_min")) {
            channel.rise_rate_pct_per_min = ch_json["rise_rate_pct_per_min"].get<double>();
        }
        if (ch_json.contains("fall_rate_pct_per_min")) {
            channel.fall_rate_pct_per_min = ch_json["fall_rate_pct_per_min"].get<double>();
        }

        // Parse demand smoothing
        if (ch_json.contains("demand_smoothing_rise_alpha")) {
            channel.demand_smoothing_rise_alpha =
                ch_json["demand_smoothing_rise_alpha"].get<double>();
        }
        if (ch_json.contains("demand_smoothing_fall_alpha")) {
            channel.demand_smoothing_fall_alpha =
                ch_json["demand_smoothing_fall_alpha"].get<double>();
        }

        // Parse decay latch
        if (ch_json.contains("decay_latch_above_pct")) {
            channel.decay_latch_above_pct = ch_json["decay_latch_above_pct"].get<double>();
        }
        if (ch_json.contains("decay_latch_pct_per_min")) {
            channel.decay_latch_pct_per_min =
                ch_json["decay_latch_pct_per_min"].get<double>();
        }

        // Parse thermal pressure parameters
        if (ch_json.contains("thermal_pressure_start_c")) {
            channel.thermal_pressure_start_c =
                ch_json["thermal_pressure_start_c"].get<double>();
        }
        if (ch_json.contains("thermal_pressure_full_c")) {
            channel.thermal_pressure_full_c =
                ch_json["thermal_pressure_full_c"].get<double>();
        }
        if (ch_json.contains("thermal_pressure_rise_pct_per_sec")) {
            channel.thermal_pressure_rise_pct_per_sec =
                ch_json["thermal_pressure_rise_pct_per_sec"].get<double>();
        }
        if (ch_json.contains("thermal_pressure_fall_pct_per_sec")) {
            channel.thermal_pressure_fall_pct_per_sec =
                ch_json["thermal_pressure_fall_pct_per_sec"].get<double>();
        }
        if (ch_json.contains("thermal_pressure_max_boost_pct")) {
            channel.thermal_pressure_max_boost_pct =
                ch_json["thermal_pressure_max_boost_pct"].get<double>();
        }

        // Parse temp blend
        if (ch_json.contains("temp_blend")) {
            try {
                channel.temp_blend = ParseTempBlend(
                    ch_json["temp_blend"].get<std::string>());
            } catch (const std::exception&) {
                channel.temp_blend = TempBlend::CpuOnly;
            }
        }

        // Parse curves
        if (ch_json.contains("curve") && ch_json["curve"].is_array()) {
            for (const auto& point_json : ch_json["curve"]) {
                if (point_json.contains("temp_c") && point_json.contains("duty_pct")) {
                    CurvePoint point;
                    point.temp_c = point_json["temp_c"].get<double>();
                    point.duty_pct = point_json["duty_pct"].get<double>();
                    channel.curve.push_back(point);
                }
            }
            // Sort by temperature
            std::sort(channel.curve.begin(), channel.curve.end(),
                     [](const CurvePoint& a, const CurvePoint& b) {
                         return a.temp_c < b.temp_c;
                     });
        }

        // Parse CPU override curve
        if (ch_json.contains("cpu_override_curve") &&
            ch_json["cpu_override_curve"].is_array()) {
            for (const auto& point_json : ch_json["cpu_override_curve"]) {
                if (point_json.contains("temp_c") && point_json.contains("duty_pct")) {
                    CurvePoint point;
                    point.temp_c = point_json["temp_c"].get<double>();
                    point.duty_pct = point_json["duty_pct"].get<double>();
                    channel.cpu_override_curve.push_back(point);
                }
            }
            // Sort by temperature
            std::sort(channel.cpu_override_curve.begin(),
                     channel.cpu_override_curve.end(),
                     [](const CurvePoint& a, const CurvePoint& b) {
                         return a.temp_c < b.temp_c;
                     });
        }

        cfg.channels.push_back(std::move(channel));
    }

    // Validate the loaded configuration
    ValidateControlLoopConfig(cfg, config_path);

    return cfg;
}

// ------------------------ ControlLoop Impl -----------------------------

struct ChannelState {
    ChannelControlConfig config;
    bool baseline_captured = false;
    std::uint8_t baseline_duty_raw = 0u;
    std::uint8_t baseline_mode_raw = 0u;
    double last_issued_pct = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point last_write_time =
        std::chrono::steady_clock::time_point{};
    bool write_active = false;
    std::chrono::steady_clock::time_point hold_deadline =
        std::chrono::steady_clock::time_point{};
    std::uint64_t total_writes = 0u;
    double last_observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double last_setpoint_pct = std::numeric_limits<double>::quiet_NaN();
    double last_raw_demand_pct = std::numeric_limits<double>::quiet_NaN();
    double smoothed_demand_pct = std::numeric_limits<double>::quiet_NaN();
    double thermal_pressure_boost_pct = 0.0;
    std::chrono::steady_clock::time_point last_evaluation_time =
        std::chrono::steady_clock::time_point{};

    // Circuit breaker for write failures
    std::uint32_t consecutive_write_failures = 0u;
    bool circuit_breaker_open = false;
    static constexpr std::uint32_t kMaxConsecutiveFailures = 5u;

    // Sensor failure detection
    std::uint32_t consecutive_sensor_failures = 0u;
    bool sensor_failed = false;
    static constexpr std::uint32_t kMaxConsecutiveSensorFailures = 3u;
    static constexpr double kSafeModeFanDuty = 100.0;  // Full speed on sensor failure
};

struct ControlLoop::Impl {
    ControlConfig base;
    ControlLoopConfig loop;
    std::filesystem::path runtime_home;
    RuntimeWritePolicy runtime_policy;
    std::vector<ChannelState> channels;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
};

ControlLoop::ControlLoop(ControlConfig base_config,
                         ControlLoopConfig loop_config,
                         std::filesystem::path runtime_home)
    : impl_(std::make_unique<Impl>()) {
    impl_->base = std::move(base_config);
    impl_->loop = std::move(loop_config);
    impl_->runtime_home = std::move(runtime_home);
    impl_->runtime_policy = ResolveRuntimeWritePolicy(&impl_->base);
    impl_->channels.reserve(impl_->loop.channels.size());
    for (const auto& ch_cfg : impl_->loop.channels) {
        ChannelState state;
        state.config = ch_cfg;
        impl_->channels.push_back(std::move(state));
    }
}

ControlLoop::~ControlLoop() = default;

namespace {

double DurationMilliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

struct ProcessResourceSample {
    bool valid_cpu = false;
    bool valid_memory = false;
    std::uint64_t total_cpu_100ns = 0u;
    std::uint64_t working_set_bytes = 0u;
    std::uint64_t private_bytes = 0u;
    std::chrono::steady_clock::time_point sampled_at =
        std::chrono::steady_clock::time_point{};
};

class TimerResolutionScope {
  public:
    explicit TimerResolutionScope(UINT period_ms)
        : period_ms_(period_ms),
          active_(timeBeginPeriod(period_ms_) == TIMERR_NOERROR) {}

    ~TimerResolutionScope() {
        if (active_) {
            timeEndPeriod(period_ms_);
        }
    }

    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

    bool active() const { return active_; }
    UINT period_ms() const { return period_ms_; }

  private:
    UINT period_ms_ = 0u;
    bool active_ = false;
};

std::uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER out{};
    out.LowPart = value.dwLowDateTime;
    out.HighPart = value.dwHighDateTime;
    return out.QuadPart;
}

std::uint32_t ActiveProcessorCount() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0u ? static_cast<std::uint32_t>(count) : 1u;
}

ProcessResourceSample SampleProcessResources() {
    ProcessResourceSample sample;
    sample.sampled_at = std::chrono::steady_clock::now();

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        sample.valid_cpu = true;
        sample.total_cpu_100ns = FileTimeToU64(kernel) + FileTimeToU64(user);
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        sample.valid_memory = true;
        sample.working_set_bytes =
            static_cast<std::uint64_t>(counters.WorkingSetSize);
        sample.private_bytes =
            static_cast<std::uint64_t>(counters.PrivateUsage);
    }

    return sample;
}

void UpdateTimingResources(RuntimeControlLoopTimingState* timing,
                           const ProcessResourceSample& previous,
                           const ProcessResourceSample& current,
                           bool have_previous,
                           std::uint32_t processor_count) {
    if (timing == nullptr) {
        return;
    }

    if (current.valid_memory) {
        timing->process_working_set_bytes = current.working_set_bytes;
        timing->process_private_bytes = current.private_bytes;
    }

    if (!have_previous || !previous.valid_cpu || !current.valid_cpu ||
        current.total_cpu_100ns < previous.total_cpu_100ns) {
        return;
    }

    const double elapsed_ms =
        DurationMilliseconds(current.sampled_at - previous.sampled_at);
    if (elapsed_ms <= 0.0) {
        return;
    }

    const double cpu_delta_ms =
        static_cast<double>(current.total_cpu_100ns -
                            previous.total_cpu_100ns) /
        10000.0;
    timing->process_cpu_delta_ms = cpu_delta_ms;
    timing->process_cpu_pct =
        (cpu_delta_ms /
         (elapsed_ms * static_cast<double>((std::max)(1u, processor_count)))) *
        100.0;
}

double JsonNumberOrZero(double value) {
    return std::isnan(value) ? 0.0 : value;
}

double GpuControlEnvelopeC(const RuntimeGpuSnapshot& gpu) {
    double envelope = (std::max)(gpu.core_c, gpu.memjn_c);
    if (gpu.hotspot_c > 0.0) {
        envelope = (std::max)(envelope, gpu.hotspot_c);
    }
    return envelope;
}

constexpr std::uint32_t kAuthorityReassertCooldownMs = 2000u;
constexpr double kAuthorityDutyTolerancePct = 3.0;

bool FanNeedsAuthorityReassert(const RuntimeFanSnapshot& fan,
                               double last_issued_pct,
                               double tolerance_pct,
                               std::string* detail) {
    if (std::isnan(last_issued_pct)) {
        return false;
    }

    const bool mode_drifted = fan.mode_raw != 0u;
    const double duty_delta = std::abs(fan.duty_percent - last_issued_pct);
    const bool duty_drifted = duty_delta > tolerance_pct;
    if (!mode_drifted && !duty_drifted) {
        return false;
    }

    if (detail != nullptr) {
        std::ostringstream stream;
        stream << "observed fan state drifted from last issued setpoint"
               << " mode_raw=" << static_cast<unsigned int>(fan.mode_raw)
               << " duty_pct=" << fan.duty_percent
               << " last_issued_pct=" << last_issued_pct
               << " tolerance_pct=" << tolerance_pct;
        *detail = stream.str();
    }
    return true;
}

bool WriteLoopStatus(const std::filesystem::path& runtime_home,
                     const std::string& mode_label,
                     const std::string& status,
                     const std::string& status_detail,
                     std::uint64_t tick_count,
                     const std::string& last_evaluation_iso,
                     const RuntimeControlLoopTimingState& timing,
                     const std::vector<ChannelState>& channels,
                     const std::string& log_csv_path,
                     const std::string& event_log_path) {
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) return false;

    const std::filesystem::path target = runtime_home / "control_runtime.json";
    const std::filesystem::path temp = runtime_home / "control_runtime.json.tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) return false;
        stream << "{\n"
               << "  \"schema_version\": 3,\n"
               << "  \"mode\": \"" << JsonEscape(mode_label) << "\",\n"
               << "  \"status\": \"" << JsonEscape(status) << "\",\n"
               << "  \"status_detail\": \"" << JsonEscape(status_detail) << "\",\n"
               << "  \"loop_tick_count\": " << tick_count << ",\n"
               << "  \"loop_last_evaluation\": \"" << JsonEscape(last_evaluation_iso) << "\",\n"
               << "  \"loop_started_wall_clock\": \""
               << JsonEscape(timing.loop_started_wall_clock) << "\",\n"
               << "  \"loop_finished_wall_clock\": \""
               << JsonEscape(timing.loop_finished_wall_clock) << "\",\n"
               << "  \"loop_work_duration_ms\": "
               << JsonNumberOrZero(timing.loop_work_duration_ms) << ",\n"
               << "  \"loop_intended_interval_ms\": "
               << timing.loop_intended_interval_ms << ",\n"
               << "  \"loop_achieved_interval_ms\": "
               << JsonNumberOrZero(timing.loop_achieved_interval_ms) << ",\n"
               << "  \"loop_slip_ms\": "
               << JsonNumberOrZero(timing.loop_slip_ms) << ",\n"
               << "  \"loop_overrun\": "
               << (timing.loop_overrun ? "true" : "false") << ",\n"
               << "  \"process_cpu_delta_ms\": "
               << JsonNumberOrZero(timing.process_cpu_delta_ms) << ",\n"
               << "  \"process_cpu_pct\": "
               << JsonNumberOrZero(timing.process_cpu_pct) << ",\n"
               << "  \"process_working_set_bytes\": "
               << timing.process_working_set_bytes << ",\n"
               << "  \"process_private_bytes\": "
               << timing.process_private_bytes << ",\n"
               << "  \"log_csv_path\": \"" << JsonEscape(log_csv_path) << "\",\n"
               << "  \"event_log_path\": \"" << JsonEscape(event_log_path) << "\",\n"
               << "  \"controlled_channels\": [";
        for (std::size_t i = 0u; i < channels.size(); ++i) {
            const ChannelState& ch = channels[i];
            if (i > 0u) stream << ",";
            stream << "\n    {\n"
                   << "      \"channel\": " << ch.config.channel << ",\n"
                   << "      \"total_writes\": " << ch.total_writes << ",\n"
                   << "      \"last_setpoint_pct\": "
                   << (std::isnan(ch.last_setpoint_pct) ? 0.0 : ch.last_setpoint_pct) << ",\n"
                   << "      \"last_raw_demand_pct\": "
                   << (std::isnan(ch.last_raw_demand_pct) ? 0.0 : ch.last_raw_demand_pct) << ",\n"
                   << "      \"last_smoothed_demand_pct\": "
                   << (std::isnan(ch.smoothed_demand_pct) ? 0.0 : ch.smoothed_demand_pct) << ",\n"
                   << "      \"last_thermal_pressure_boost_pct\": "
                   << ch.thermal_pressure_boost_pct << ",\n"
                   << "      \"last_observed_temp_c\": "
                   << (std::isnan(ch.last_observed_temp_c) ? 0.0 : ch.last_observed_temp_c) << ",\n"
                   << "      \"baseline_captured\": "
                   << (ch.baseline_captured ? "true" : "false") << "\n"
                   << "    }";
        }
        if (!channels.empty()) stream << "\n  ";
        stream << "]\n}\n";
        stream.flush();
        if (stream.fail()) return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<RuntimeControlChannelLogState> BuildChannelLogStates(
    const std::vector<ChannelState>& channels) {
    std::vector<RuntimeControlChannelLogState> log_states;
    log_states.reserve(channels.size());
    for (const auto& channel : channels) {
        RuntimeControlChannelLogState state;
        state.channel = channel.config.channel;
        state.observed_temp_c = channel.last_observed_temp_c;
        state.setpoint_pct = channel.last_setpoint_pct;
        state.thermal_pressure_boost_pct =
            channel.thermal_pressure_boost_pct;
        state.total_writes = channel.total_writes;
        state.write_active = channel.write_active;
        state.baseline_captured = channel.baseline_captured;
        log_states.push_back(state);
    }
    return log_states;
}

}  // namespace

int ControlLoop::RunUntilStopped(const std::atomic<bool>& stop_flag) {
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(impl_->runtime_home).string();
    RuntimeCsvLogger csv_logger(
        impl_->runtime_home,
        impl_->base.log_rotate_hours,
        impl_->base.log_retain_days);
    std::string log_csv_path;
    if (csv_logger.Open("control-loop", BuildControlLoopCsvHeader())) {
        log_csv_path = csv_logger.active_archive_path().string();
    }
    RuntimeControlLoopTimingState last_timing;
    last_timing.loop_intended_interval_ms = impl_->loop.poll_tick_ms;

    AmdReader amd_reader;
    GpuReader gpu_reader;

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(impl_->runtime_policy);
    } catch (const std::exception& error) {
        AppendRuntimeEvent(
            impl_->runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.init_failed",
                .detail = std::string("direct writer init failed: ") +
                          error.what(),
                .success = false,
            });
        WriteLoopStatus(impl_->runtime_home, "control-loop", "failed",
                        std::string("direct writer init failed: ") + error.what(),
                        0u, FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, impl_->channels, log_csv_path, event_log_path);
        return 1;
    }

    TimerResolutionScope timer_resolution(1u);

    std::uint64_t tick_count = 0u;
    {
        std::ostringstream detail;
        detail << "direct telemetry active; fan_writer="
               << fan_writer->BackendLabel()
               << " policy="
               << (impl_->runtime_policy.present
                       ? impl_->runtime_policy.source_path.string()
                       : std::string("(none)"))
               << " poll_tick_ms="
               << impl_->loop.poll_tick_ms
               << " cooldown_ms=" << impl_->loop.write_cooldown_ms
               << " deadband_pct=" << impl_->loop.deadband_pct
               << " hold_ms=" << impl_->loop.control_hold_ms
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"))
               << " channels=" << impl_->channels.size() << ")";
        WriteLoopStatus(impl_->runtime_home, "control-loop", "running",
                        detail.str(), tick_count,
                        FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, impl_->channels, log_csv_path, event_log_path);
    }
    AppendRuntimeEvent(
        impl_->runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.start",
            .detail = "control-loop started",
            .success = true,
        });

    bool fatal_restore_timeout = false;
    std::uint32_t fatal_restore_channel = 0u;
    std::chrono::steady_clock::time_point previous_tick_start;
    bool have_previous_tick_start = false;
    const std::uint32_t processor_count = ActiveProcessorCount();
    ProcessResourceSample resource_window_sample = SampleProcessResources();
    bool have_resource_window_sample = resource_window_sample.valid_cpu ||
        resource_window_sample.valid_memory;
    double last_process_cpu_delta_ms =
        std::numeric_limits<double>::quiet_NaN();
    double last_process_cpu_pct = std::numeric_limits<double>::quiet_NaN();

    while (!stop_flag.load()) {
        ++tick_count;
        const auto tick_started_steady = std::chrono::steady_clock::now();
        const auto tick_started_wall = std::chrono::system_clock::now();
        double achieved_interval_ms =
            std::numeric_limits<double>::quiet_NaN();
        if (have_previous_tick_start) {
            achieved_interval_ms =
                DurationMilliseconds(tick_started_steady - previous_tick_start);
        }
        previous_tick_start = tick_started_steady;
        have_previous_tick_start = true;
        const auto now_steady = tick_started_steady;
        const auto eval_iso = FormatLocalIso8601(tick_started_wall);

        RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *fan_writer, impl_->runtime_policy);
        const bool runtime_snapshot_available =
            RuntimeSnapshotHasTelemetry(runtime_snapshot);

        if (csv_logger.MaybeRotate()) {
            log_csv_path = csv_logger.active_archive_path().string();
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.log_rotated",
                    .detail = "telemetry log rotated",
                    .tick_count = tick_count,
                    .success = true,
                });
        }

        // Extract CPU temp.
        TempInputs temp_inputs;
        if (runtime_snapshot_available) {
            const double cpu_c = FindRuntimeAmdSensorTemperature(
                runtime_snapshot, impl_->loop.cpu_temp_label);
            if (!std::isnan(cpu_c)) {
                temp_inputs.cpu_c = cpu_c;
                temp_inputs.cpu_available = true;
                temp_inputs.cpu_label = impl_->loop.cpu_temp_label;
            }
        }
        if (runtime_snapshot.gpu.available) {
            temp_inputs.gpu_c = GpuControlEnvelopeC(runtime_snapshot.gpu);
            temp_inputs.gpu_available = true;
            temp_inputs.gpu_label = "gpu_envelope";
        }

        if (runtime_snapshot_available) {
            WriteRuntimeSnapshotFile(impl_->runtime_home, runtime_snapshot);
        }

        // Per-channel decisions.
        for (auto& channel : impl_->channels) {
            const std::uint32_t effective_cooldown_ms =
                EffectiveWriteCooldownMs(impl_->loop, channel.config);
            const double effective_deadband_pct =
                EffectiveDeadbandPct(impl_->loop, channel.config);
            const std::uint32_t effective_hold_ms =
                EffectiveControlHoldMs(impl_->loop, channel.config);

            if (!channel.baseline_captured) {
                if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                        runtime_snapshot, channel.config.channel)) {
                    channel.baseline_duty_raw = fan->duty_raw;
                    channel.baseline_mode_raw = fan->mode_raw;
                    channel.baseline_captured = true;
                    AppendRuntimeEvent(
                        impl_->runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.baseline_captured",
                            .detail = "captured baseline for control channel",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = true,
                        });
                }
            }

            if (channel.write_active &&
                effective_hold_ms > 0u &&
                now_steady >= channel.hold_deadline &&
                channel.baseline_captured) {
                const FanWriteResult restore_result =
                    fan_writer->RestoreSavedState(channel.config.channel,
                                                  channel.baseline_duty_raw,
                                                  channel.baseline_mode_raw,
                                                  impl_->base.restore_timeout_ms);
                if (restore_result) {
                    AppendRuntimeEvent(
                        impl_->runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.restore_applied",
                            .detail = "restored channel to captured baseline",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = true,
                        });
                    channel.write_active = false;
                    channel.last_issued_pct =
                        std::numeric_limits<double>::quiet_NaN();
                    try {
                        RemovePendingWrite(impl_->runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but don't fail
                        AppendRuntimeEvent(
                            impl_->runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sidecar_remove_warning",
                                .detail = std::string("best-effort sidecar removal after restore failed: ") + e.what(),
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .success = false,
                            });
                    }
                } else {
                    AppendRuntimeEvent(
                        impl_->runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.restore_failed",
                            .detail = restore_result.detail,
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = false,
                        });
                    if (restore_result.error == FanWriteError::kTimedOut) {
                        fatal_restore_timeout = true;
                        fatal_restore_channel = channel.config.channel;
                        break;
                    }
                }
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_steady - channel.last_write_time).count();
            const std::uint64_t elapsed_ms = elapsed > 0
                ? static_cast<std::uint64_t>(elapsed)
                : 0u;

            const auto evaluation_elapsed =
                channel.last_evaluation_time ==
                        std::chrono::steady_clock::time_point{}
                    ? static_cast<std::uint64_t>(impl_->loop.poll_tick_ms)
                    : static_cast<std::uint64_t>(
                          (std::max)(
                              0ll,
                              std::chrono::duration_cast<
                                  std::chrono::milliseconds>(
                                  now_steady - channel.last_evaluation_time)
                                  .count()));
            channel.last_evaluation_time = now_steady;

            const double blended = BlendTemps(temp_inputs, channel.config.temp_blend);
            const bool primary_available = blended >= -100.0;
            double raw_desired_setpoint = std::numeric_limits<double>::quiet_NaN();
            double observed_temp_c = std::numeric_limits<double>::quiet_NaN();

            // Sensor failure detection
            if (!primary_available) {
                ++channel.consecutive_sensor_failures;
                if (channel.consecutive_sensor_failures >= ChannelState::kMaxConsecutiveSensorFailures) {
                    if (!channel.sensor_failed) {
                        channel.sensor_failed = true;
                        AppendRuntimeEvent(
                            impl_->runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sensor_failure_detected",
                                .detail = "sensor failure detected, entering safe mode (100% duty)",
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .success = false,
                            });
                    }
                    // Safe mode: force full speed
                    raw_desired_setpoint = ChannelState::kSafeModeFanDuty;
                    observed_temp_c = std::numeric_limits<double>::quiet_NaN();
                }
            } else {
                // Sensor recovered
                if (channel.consecutive_sensor_failures > 0 || channel.sensor_failed) {
                    if (channel.sensor_failed) {
                        AppendRuntimeEvent(
                            impl_->runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sensor_recovered",
                                .detail = "sensor recovered, resuming normal control",
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .observed_temp_c = blended,
                                .success = true,
                            });
                    }
                    channel.consecutive_sensor_failures = 0u;
                    channel.sensor_failed = false;
                }

                raw_desired_setpoint = LookupCurve(
                    channel.config.curve, blended, channel.config.min_duty_pct,
                    channel.config.curve_shape);
                observed_temp_c = blended;
            }
            if (!channel.config.cpu_override_curve.empty() &&
                temp_inputs.cpu_available) {
                const double cpu_setpoint = LookupCurve(
                    channel.config.cpu_override_curve, temp_inputs.cpu_c,
                    channel.config.min_duty_pct, channel.config.curve_shape);
                if (std::isnan(raw_desired_setpoint) ||
                    cpu_setpoint > raw_desired_setpoint) {
                    raw_desired_setpoint = cpu_setpoint;
                    observed_temp_c = temp_inputs.cpu_c;
                }
            }
            channel.last_observed_temp_c = observed_temp_c;
            channel.last_raw_demand_pct = raw_desired_setpoint;
            if (std::isnan(raw_desired_setpoint)) continue;  // no valid input

            const double smoothed_base_setpoint = ApplyDemandSmoothing(
                raw_desired_setpoint, channel.smoothed_demand_pct,
                evaluation_elapsed, channel.config);
            channel.smoothed_demand_pct = smoothed_base_setpoint;
            channel.thermal_pressure_boost_pct = UpdateThermalPressureBoost(
                observed_temp_c, channel.thermal_pressure_boost_pct,
                evaluation_elapsed, channel.config);

            const double desired_setpoint = std::clamp(
                smoothed_base_setpoint + channel.thermal_pressure_boost_pct,
                0.0, 100.0);

            const double setpoint = RateLimitSetpoint(
                desired_setpoint, channel.last_issued_pct, elapsed_ms,
                channel.config.rise_rate_pct_per_min,
                channel.config.fall_rate_pct_per_min);
            channel.last_setpoint_pct = setpoint;

            std::string authority_detail;
            bool authority_reassert = false;
            if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                    runtime_snapshot, channel.config.channel)) {
                authority_reassert =
                    effective_hold_ms == 0u &&
                    FanNeedsAuthorityReassert(
                        *fan, channel.last_issued_pct,
                        (std::max)(kAuthorityDutyTolerancePct,
                                   effective_deadband_pct),
                        &authority_detail);
            }

            if (!std::isnan(channel.last_issued_pct)) {
                const double delta = std::abs(setpoint - channel.last_issued_pct);
                if (!authority_reassert && delta < effective_deadband_pct) {
                    continue;
                }
            }

            if (std::isnan(channel.last_issued_pct)) {
                // First write for this channel — allow immediately.
            } else if (static_cast<std::uint64_t>(elapsed) <
                       static_cast<std::uint64_t>(
                           authority_reassert
                               ? (std::min)(effective_cooldown_ms,
                                            kAuthorityReassertCooldownMs)
                               : effective_cooldown_ms)) {
                continue;
            }

            // Ensure we have a baseline before recording the sidecar.
            if (!channel.baseline_captured) {
                continue;  // skip until snapshot provides baseline
            }

            if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                    runtime_snapshot, channel.config.channel)) {
                if (!fan->effective_write_allowed) {
                    continue;
                }
            }

            // Skip write if circuit breaker is open
            if (channel.circuit_breaker_open) {
                continue;
            }

            // Record sidecar entry.
            PendingWriteEntry entry;
            entry.channel = channel.config.channel;
            entry.baseline_duty_raw = channel.baseline_duty_raw;
            entry.baseline_mode_raw = channel.baseline_mode_raw;
            entry.target_pct = setpoint;
            entry.requested_hold_ms = effective_hold_ms;
            entry.started_iso = eval_iso;
            entry.child_pid = 0u;
            try {
                UpsertPendingWrite(impl_->runtime_home, entry);
            } catch (const std::exception& e) {
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sidecar_upsert_failed",
                        .detail = std::string("failed to write sidecar entry before fan write: ") + e.what(),
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
                continue;
            }

            const FanWriteResult write_result =
                fan_writer->ApplyDuty(channel.config.channel, setpoint);
            if (!write_result) {
                // Circuit breaker: track consecutive failures
                ++channel.consecutive_write_failures;
                if (channel.consecutive_write_failures >= ChannelState::kMaxConsecutiveFailures) {
                    if (!channel.circuit_breaker_open) {
                        channel.circuit_breaker_open = true;
                        AppendRuntimeEvent(
                            impl_->runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.circuit_breaker_opened",
                                .detail = "circuit breaker opened after repeated write failures",
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .observed_temp_c = observed_temp_c,
                                .setpoint_pct = setpoint,
                                .success = false,
                            });
                    }
                }

                if (write_result.error == FanWriteError::kPolicyRefused) {
                    try {
                        RemovePendingWrite(impl_->runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but continue
                        AppendRuntimeEvent(
                            impl_->runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sidecar_remove_warning",
                                .detail = std::string("best-effort sidecar removal after policy refusal failed: ") + e.what(),
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .success = false,
                            });
                    }
                }
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.write_failed",
                        .detail = write_result.detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c = observed_temp_c,
                        .setpoint_pct = setpoint,
                        .success = false,
                    });
                continue;
            }

            // Success: reset circuit breaker
            if (channel.consecutive_write_failures > 0 || channel.circuit_breaker_open) {
                if (channel.circuit_breaker_open) {
                    AppendRuntimeEvent(
                        impl_->runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.circuit_breaker_closed",
                            .detail = "circuit breaker closed after successful write",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .observed_temp_c = observed_temp_c,
                            .setpoint_pct = setpoint,
                            .success = true,
                        });
                }
                channel.consecutive_write_failures = 0u;
                channel.circuit_breaker_open = false;
            }
            if (authority_reassert) {
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.authority_reasserted",
                        .detail = authority_detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c = observed_temp_c,
                        .setpoint_pct = setpoint,
                        .success = true,
                    });
            }
            channel.write_active = true;
            channel.hold_deadline =
                effective_hold_ms == 0u
                    ? std::chrono::steady_clock::time_point::max()
                    : now_steady + std::chrono::milliseconds(effective_hold_ms);
            channel.last_issued_pct = setpoint;
            channel.last_write_time = now_steady;
            ++channel.total_writes;
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.write_applied",
                    .detail = "applied control-loop setpoint",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .observed_temp_c = observed_temp_c,
                    .setpoint_pct = setpoint,
                    .success = true,
                });
        }

        const auto tick_finished_steady = std::chrono::steady_clock::now();
        const auto tick_finished_wall = std::chrono::system_clock::now();
        const double work_duration_ms =
            DurationMilliseconds(tick_finished_steady - tick_started_steady);
        RuntimeControlLoopTimingState tick_timing;
        tick_timing.loop_started_wall_clock = eval_iso;
        tick_timing.loop_finished_wall_clock =
            FormatLocalIso8601(tick_finished_wall);
        tick_timing.loop_work_duration_ms = work_duration_ms;
        tick_timing.loop_intended_interval_ms = impl_->loop.poll_tick_ms;
        tick_timing.loop_achieved_interval_ms = achieved_interval_ms;
        tick_timing.loop_slip_ms = std::isnan(achieved_interval_ms)
            ? std::numeric_limits<double>::quiet_NaN()
            : achieved_interval_ms -
                  static_cast<double>(impl_->loop.poll_tick_ms);
        tick_timing.loop_overrun =
            work_duration_ms > static_cast<double>(impl_->loop.poll_tick_ms);
        const ProcessResourceSample current_resource_sample =
            SampleProcessResources();
        if (current_resource_sample.valid_memory) {
            tick_timing.process_working_set_bytes =
                current_resource_sample.working_set_bytes;
            tick_timing.process_private_bytes =
                current_resource_sample.private_bytes;
        }
        const double resource_window_ms =
            have_resource_window_sample
                ? DurationMilliseconds(current_resource_sample.sampled_at -
                                       resource_window_sample.sampled_at)
                : 0.0;
        if (resource_window_ms >= 1000.0) {
            RuntimeControlLoopTimingState resource_timing;
            UpdateTimingResources(&resource_timing,
                                  resource_window_sample,
                                  current_resource_sample,
                                  have_resource_window_sample,
                                  processor_count);
            last_process_cpu_delta_ms = resource_timing.process_cpu_delta_ms;
            last_process_cpu_pct = resource_timing.process_cpu_pct;
            resource_window_sample = current_resource_sample;
            have_resource_window_sample =
                current_resource_sample.valid_cpu ||
                current_resource_sample.valid_memory;
        }
        tick_timing.process_cpu_delta_ms = last_process_cpu_delta_ms;
        tick_timing.process_cpu_pct = last_process_cpu_pct;
        last_timing = tick_timing;

        if (fatal_restore_timeout) {
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.abort",
                    .detail = "restore timed out; aborting control-loop",
                    .channel = fatal_restore_channel,
                    .tick_count = tick_count,
                    .success = false,
                });
            break;
        }

        if (csv_logger.is_open()) {
            csv_logger.WriteRow(
                BuildControlLoopCsvRow(
                    runtime_snapshot,
                    tick_count,
                    last_timing,
                    BuildChannelLogStates(impl_->channels)));
        }

        // Rate-limit status file writes to reduce disk I/O
        // Write every 10 ticks (e.g., every 2 seconds at 200ms tick)
        constexpr std::uint32_t kStatusUpdateIntervalTicks = 10u;
        const bool should_write_status = (tick_count % kStatusUpdateIntervalTicks) == 0u;

        if (should_write_status) {
            std::ostringstream td;
            td << "tick poll_tick_ms=" << impl_->loop.poll_tick_ms
               << " cooldown=" << impl_->loop.write_cooldown_ms
               << " deadband=" << impl_->loop.deadband_pct
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"));
            WriteLoopStatus(impl_->runtime_home, "control-loop", "running",
                            td.str(), tick_count, eval_iso, last_timing,
                            impl_->channels, log_csv_path, event_log_path);
        }

        const auto next_tick_deadline =
            tick_started_steady +
            std::chrono::milliseconds(impl_->loop.poll_tick_ms);
        const auto after_tick_steady = std::chrono::steady_clock::now();
        if (after_tick_steady < next_tick_deadline) {
            std::unique_lock<std::mutex> lock(impl_->wake_mutex);
            impl_->wake_cv.wait_until(
                lock, next_tick_deadline,
                [&stop_flag] { return stop_flag.load(); });
        }
    }

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteLoopStatus(impl_->runtime_home, "control-loop", "shutdown",
                    "stop requested", tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, impl_->channels, log_csv_path, event_log_path);
    AppendRuntimeEvent(
        impl_->runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.shutdown_requested",
            .detail = "stop requested",
            .tick_count = tick_count,
            .success = true,
        });
    bool restore_failure = false;
    for (auto& channel : impl_->channels) {
        if (fatal_restore_timeout && channel.write_active) {
            restore_failure = true;
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_restore_skipped",
                    .detail = "skipped shutdown restore after earlier restore timeout",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = false,
                });
            continue;
        }
        if (channel.write_active && channel.baseline_captured) {
            const FanWriteResult restore_result =
                fan_writer->RestoreSavedState(channel.config.channel,
                                              channel.baseline_duty_raw,
                                              channel.baseline_mode_raw,
                                              impl_->base.restore_timeout_ms);
            if (!restore_result) {
                restore_failure = true;
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.shutdown_restore_failed",
                        .detail = restore_result.detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
                continue;
            }
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_restore_applied",
                    .detail = "restored channel during shutdown",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = true,
                });
        }
        try {
            RemovePendingWrite(impl_->runtime_home, channel.config.channel);
        } catch (const std::exception& e) {
            restore_failure = true;
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_sidecar_remove_failed",
                    .detail = std::string("sidecar removal during shutdown failed: ") + e.what(),
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = false,
                });
        }
    }
    WriteLoopStatus(impl_->runtime_home, "control-loop", "shutdown",
                    restore_failure ? "restore failed"
                                    : "channels restored",
                    tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, impl_->channels, log_csv_path, event_log_path);
    AppendRuntimeEvent(
        impl_->runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.shutdown",
            .detail = restore_failure ? "restore failed"
                                      : "channels restored",
            .tick_count = tick_count,
            .success = !restore_failure,
        });
    return restore_failure ? 1 : 0;
}

}  // namespace svg_mb_control
