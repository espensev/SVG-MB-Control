// Configuration parsing and validation for the control loop. Split out of
// control_loop.cpp so the steady-state loop engine (ControlLoop::Impl) and the
// config load/validate path are independent translation units. The public
// declaration of LoadControlLoopConfig stays in control_loop.h.

#include "control_loop.h"

#include "control_config.h"
#include "json_io.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace svg_mb_control {

namespace {

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
                  const std::string& curve_name,
                  bool allow_empty = false) {
    if (curve.empty()) {
        if (allow_empty) {
            return;
        }
        throw std::runtime_error(curve_name + " must not be empty");
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
    ValidatePercentage(ch.max_setpoint_step_pct,
                      prefix + " max_setpoint_step_pct", true);

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

    if (!std::isnan(ch.cpu_low_soak_start_c) ||
        !std::isnan(ch.cpu_low_soak_full_c) ||
        !std::isnan(ch.cpu_low_soak_release_c) ||
        !std::isnan(ch.cpu_low_soak_rise_pct_per_min) ||
        !std::isnan(ch.cpu_low_soak_fall_pct_per_min) ||
        !std::isnan(ch.cpu_low_soak_max_boost_pct)) {
        if (std::isnan(ch.cpu_low_soak_start_c) ||
            std::isnan(ch.cpu_low_soak_full_c) ||
            std::isnan(ch.cpu_low_soak_release_c) ||
            std::isnan(ch.cpu_low_soak_rise_pct_per_min) ||
            std::isnan(ch.cpu_low_soak_fall_pct_per_min) ||
            std::isnan(ch.cpu_low_soak_max_boost_pct)) {
            throw std::runtime_error(
                prefix + " cpu_low_soak requires the complete field set");
        }

        ValidatePositive(ch.cpu_low_soak_start_c,
                        prefix + " cpu_low_soak_start_c");
        ValidatePositive(ch.cpu_low_soak_full_c,
                        prefix + " cpu_low_soak_full_c");
        ValidatePositive(ch.cpu_low_soak_release_c,
                        prefix + " cpu_low_soak_release_c");
        ValidatePositive(ch.cpu_low_soak_rise_pct_per_min,
                        prefix + " cpu_low_soak_rise_pct_per_min");
        ValidatePositive(ch.cpu_low_soak_fall_pct_per_min,
                        prefix + " cpu_low_soak_fall_pct_per_min");
        ValidatePercentage(ch.cpu_low_soak_max_boost_pct,
                          prefix + " cpu_low_soak_max_boost_pct");

        if (ch.cpu_low_soak_full_c <= ch.cpu_low_soak_start_c) {
            throw std::runtime_error(
                prefix + " cpu_low_soak_full_c must be > start_c");
        }
        if (ch.cpu_low_soak_release_c > ch.cpu_low_soak_start_c) {
            throw std::runtime_error(
                prefix + " cpu_low_soak_release_c must be <= start_c");
        }
        if (ch.cpu_low_soak_max_boost_pct > 10.0) {
            throw std::runtime_error(
                prefix + " cpu_low_soak_max_boost_pct must be <= 10");
        }
    }

    if (ch.low_band_stage > 0u ||
        !std::isnan(ch.low_band_debt_threshold) ||
        !std::isnan(ch.low_band_max_boost_pct) ||
        ch.low_band_hold_ms > 0u) {
        if (ch.low_band_stage == 0u) {
            throw std::runtime_error(
                prefix + " low_band_stage must be > 0 when low-band fields are set");
        }
        ValidatePercentage(ch.low_band_debt_threshold,
                          prefix + " low_band_debt_threshold");
        ValidatePercentage(ch.low_band_max_boost_pct,
                          prefix + " low_band_max_boost_pct");
        if (ch.low_band_debt_threshold <= 0.0 ||
            ch.low_band_debt_threshold >= 1.0) {
            throw std::runtime_error(
                prefix + " low_band_debt_threshold must be in (0, 1)");
        }
        if (ch.low_band_max_boost_pct > 10.0) {
            throw std::runtime_error(
                prefix + " low_band_max_boost_pct must be <= 10");
        }
    }

    ValidateCurve(ch.curve, prefix + " curve");
    ValidateCurve(ch.cpu_override_curve, prefix + " cpu_override_curve", true);
}

void ValidateLowBandConfig(const LowBandControlConfig& cfg) {
    if (!cfg.enabled) {
        return;
    }

    ValidatePositive(cfg.cpu_start_c, "low_band.cpu_start_c");
    ValidatePositive(cfg.cpu_full_c, "low_band.cpu_full_c");
    ValidatePositive(cfg.cpu_release_c, "low_band.cpu_release_c");
    ValidatePositive(cfg.gpu_start_c, "low_band.gpu_start_c");
    ValidatePositive(cfg.gpu_full_c, "low_band.gpu_full_c");
    ValidatePositive(cfg.gpu_release_c, "low_band.gpu_release_c");
    ValidatePositive(cfg.cpu_weight, "low_band.cpu_weight");
    ValidatePositive(cfg.gpu_weight, "low_band.gpu_weight");
    ValidatePositive(cfg.rise_per_min, "low_band.rise_per_min");
    ValidatePositive(cfg.fall_per_min, "low_band.fall_per_min");
    ValidatePositive(cfg.stage_rise_pct_per_min,
                    "low_band.stage_rise_pct_per_min");
    ValidatePositive(cfg.stage_fall_pct_per_min,
                    "low_band.stage_fall_pct_per_min");

    if (cfg.cpu_full_c <= cfg.cpu_start_c) {
        throw std::runtime_error("low_band.cpu_full_c must be > cpu_start_c");
    }
    if (cfg.gpu_full_c <= cfg.gpu_start_c) {
        throw std::runtime_error("low_band.gpu_full_c must be > gpu_start_c");
    }
    if (cfg.cpu_release_c > cfg.cpu_start_c) {
        throw std::runtime_error("low_band.cpu_release_c must be <= cpu_start_c");
    }
    if (cfg.gpu_release_c > cfg.gpu_start_c) {
        throw std::runtime_error("low_band.gpu_release_c must be <= gpu_start_c");
    }
    if (cfg.rise_per_min <= 0.0) {
        throw std::runtime_error("low_band.rise_per_min must be > 0");
    }
    if (cfg.stage_rise_pct_per_min <= 0.0) {
        throw std::runtime_error(
            "low_band.stage_rise_pct_per_min must be > 0");
    }
    if (cfg.stage_spacing_ms == 0u) {
        throw std::runtime_error("low_band.stage_spacing_ms must be > 0");
    }
    if (cfg.evidence_write_interval_ms == 0u) {
        throw std::runtime_error(
            "low_band.evidence_write_interval_ms must be > 0");
    }
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
    ValidateLowBandConfig(cfg.low_band);

    if (cfg.channels.empty()) {
        throw std::runtime_error(
            "control_loop config has empty channels array: " +
            config_path.string());
    }

    for (std::size_t i = 0; i < cfg.channels.size(); ++i) {
        ValidateChannelConfig(cfg.channels[i], static_cast<std::uint32_t>(i));
    }
}

}  // namespace

// ------------------------ Config loader --------------------------------

ControlLoopConfig LoadControlLoopConfig(
    const std::filesystem::path& config_path) {
    const nlohmann::json root = ReadJsonFile(config_path, "control config");

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

    if (loop_json.contains("low_band") && loop_json["low_band"].is_object()) {
        const auto& low_json = loop_json["low_band"];
        cfg.low_band.enabled =
            low_json.value("enabled", cfg.low_band.enabled);
        cfg.low_band.cpu_start_c =
            low_json.value("cpu_start_c", cfg.low_band.cpu_start_c);
        cfg.low_band.cpu_full_c =
            low_json.value("cpu_full_c", cfg.low_band.cpu_full_c);
        cfg.low_band.cpu_release_c =
            low_json.value("cpu_release_c", cfg.low_band.cpu_release_c);
        cfg.low_band.gpu_start_c =
            low_json.value("gpu_start_c", cfg.low_band.gpu_start_c);
        cfg.low_band.gpu_full_c =
            low_json.value("gpu_full_c", cfg.low_band.gpu_full_c);
        cfg.low_band.gpu_release_c =
            low_json.value("gpu_release_c", cfg.low_band.gpu_release_c);
        cfg.low_band.cpu_weight =
            low_json.value("cpu_weight", cfg.low_band.cpu_weight);
        cfg.low_band.gpu_weight =
            low_json.value("gpu_weight", cfg.low_band.gpu_weight);
        cfg.low_band.rise_per_min =
            low_json.value("rise_per_min", cfg.low_band.rise_per_min);
        cfg.low_band.fall_per_min =
            low_json.value("fall_per_min", cfg.low_band.fall_per_min);
        cfg.low_band.stage_rise_pct_per_min = low_json.value(
            "stage_rise_pct_per_min",
            cfg.low_band.stage_rise_pct_per_min);
        cfg.low_band.stage_fall_pct_per_min = low_json.value(
            "stage_fall_pct_per_min",
            cfg.low_band.stage_fall_pct_per_min);
        cfg.low_band.stage_spacing_ms =
            low_json.value("stage_spacing_ms", cfg.low_band.stage_spacing_ms);
        cfg.low_band.evidence_write_interval_ms = low_json.value(
            "evidence_write_interval_ms",
            cfg.low_band.evidence_write_interval_ms);
    }

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
        if (ch_json.contains("max_setpoint_step_pct")) {
            channel.max_setpoint_step_pct =
                ch_json["max_setpoint_step_pct"].get<double>();
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

        if (ch_json.contains("cpu_low_soak_start_c")) {
            channel.cpu_low_soak_start_c =
                ch_json["cpu_low_soak_start_c"].get<double>();
        }
        if (ch_json.contains("cpu_low_soak_full_c")) {
            channel.cpu_low_soak_full_c =
                ch_json["cpu_low_soak_full_c"].get<double>();
        }
        if (ch_json.contains("cpu_low_soak_release_c")) {
            channel.cpu_low_soak_release_c =
                ch_json["cpu_low_soak_release_c"].get<double>();
        }
        if (ch_json.contains("cpu_low_soak_rise_pct_per_min")) {
            channel.cpu_low_soak_rise_pct_per_min =
                ch_json["cpu_low_soak_rise_pct_per_min"].get<double>();
        }
        if (ch_json.contains("cpu_low_soak_fall_pct_per_min")) {
            channel.cpu_low_soak_fall_pct_per_min =
                ch_json["cpu_low_soak_fall_pct_per_min"].get<double>();
        }
        if (ch_json.contains("cpu_low_soak_max_boost_pct")) {
            channel.cpu_low_soak_max_boost_pct =
                ch_json["cpu_low_soak_max_boost_pct"].get<double>();
        }

        channel.low_band_stage =
            ch_json.value("low_band_stage", channel.low_band_stage);
        if (ch_json.contains("low_band_debt_threshold")) {
            channel.low_band_debt_threshold =
                ch_json["low_band_debt_threshold"].get<double>();
        }
        channel.low_band_hold_ms =
            ch_json.value("low_band_hold_ms", channel.low_band_hold_ms);
        if (ch_json.contains("low_band_max_boost_pct")) {
            channel.low_band_max_boost_pct =
                ch_json["low_band_max_boost_pct"].get<double>();
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

}  // namespace svg_mb_control
