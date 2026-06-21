// Configuration parsing and validation for the control loop. Split out of
// control_loop.cpp so the steady-state loop engine (ControlLoop::Impl) and the
// config load/validate path are independent translation units. The public
// declaration of LoadControlLoopConfig stays in control_loop.h.

#include "control_loop.h"

#include "control_config.h"
#include "json_io.h"
#include "profile_composition.h"

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

void ReadOptionalDouble(const nlohmann::json& json,
                        const std::string& key,
                        double& target) {
    if (json.contains(key)) {
        target = json[key].get<double>();
    }
}

// JSON key suffix for the rise/fall rate fields. BelowStart stages use
// percent-per-second (matches today's on-disk schema for thermal_pressure,
// midband_pressure, gpu_airflow); ExplicitRelease stages use
// percent-per-minute (cpu_low_soak).
const char* RateRiseKeySuffix(const BoostStageSpec& spec) {
    return spec.rate_unit == BoostRateUnit::PerSec
        ? "_rise_pct_per_sec"
        : "_rise_pct_per_min";
}
const char* RateFallKeySuffix(const BoostStageSpec& spec) {
    return spec.rate_unit == BoostRateUnit::PerSec
        ? "_fall_pct_per_sec"
        : "_fall_pct_per_min";
}

void LoadBoostStageConfig(const nlohmann::json& json,
                          const BoostStageSpec& spec,
                          BoostStageConfig& cfg) {
    const std::string prefix(spec.name);
    ReadOptionalDouble(json, prefix + "_start_c", cfg.start_c);
    ReadOptionalDouble(json, prefix + "_full_c", cfg.full_c);
    if (spec.release_mode == BoostReleaseMode::ExplicitRelease) {
        ReadOptionalDouble(json, prefix + "_release_c", cfg.release_c);
    }
    ReadOptionalDouble(json, prefix + RateRiseKeySuffix(spec),
                       cfg.rise_per_unit);
    ReadOptionalDouble(json, prefix + RateFallKeySuffix(spec),
                       cfg.fall_per_unit);
    ReadOptionalDouble(json, prefix + "_max_boost_pct", cfg.max_boost_pct);
}

bool AnyBoostStageFieldSet(const BoostStageConfig& cfg,
                           const BoostStageSpec& spec) {
    return !std::isnan(cfg.start_c) ||
           !std::isnan(cfg.full_c) ||
           (spec.release_mode == BoostReleaseMode::ExplicitRelease &&
            !std::isnan(cfg.release_c)) ||
           !std::isnan(cfg.rise_per_unit) ||
           !std::isnan(cfg.fall_per_unit) ||
           !std::isnan(cfg.max_boost_pct);
}

void ValidateBoostStageConfig(const BoostStageConfig& cfg,
                              const BoostStageSpec& spec,
                              const std::string& channel_prefix) {
    if (!AnyBoostStageFieldSet(cfg, spec)) {
        return;
    }

    const std::string stage(spec.name);
    const std::string rise_key =
        stage + RateRiseKeySuffix(spec);
    const std::string fall_key =
        stage + RateFallKeySuffix(spec);
    const std::string max_key = stage + "_max_boost_pct";

    if (spec.release_mode == BoostReleaseMode::ExplicitRelease) {
        // Strict: any field set requires the complete set (matches the
        // legacy cpu_low_soak contract).
        if (std::isnan(cfg.start_c) || std::isnan(cfg.full_c) ||
            std::isnan(cfg.release_c) ||
            std::isnan(cfg.rise_per_unit) ||
            std::isnan(cfg.fall_per_unit) ||
            std::isnan(cfg.max_boost_pct)) {
            throw std::runtime_error(
                channel_prefix + " " + stage +
                " requires the complete field set");
        }
        ValidatePositive(cfg.start_c, channel_prefix + " " + stage + "_start_c");
        ValidatePositive(cfg.full_c, channel_prefix + " " + stage + "_full_c");
        ValidatePositive(cfg.release_c,
                         channel_prefix + " " + stage + "_release_c");
        ValidatePositive(cfg.rise_per_unit, channel_prefix + " " + rise_key);
        ValidatePositive(cfg.fall_per_unit, channel_prefix + " " + fall_key);
        ValidatePercentage(cfg.max_boost_pct, channel_prefix + " " + max_key);
        if (cfg.full_c <= cfg.start_c) {
            throw std::runtime_error(
                channel_prefix + " " + stage + "_full_c must be > start_c");
        }
        if (cfg.release_c > cfg.start_c) {
            throw std::runtime_error(
                channel_prefix + " " + stage +
                "_release_c must be <= start_c");
        }
        if (cfg.max_boost_pct > 10.0) {
            throw std::runtime_error(
                channel_prefix + " " + stage + "_max_boost_pct must be <= 10");
        }
        return;
    }

    // BelowStart: lenient. Require start_c + full_c; rise/fall/max may
    // stay NaN (UpdateBoostStage treats the spec as inert in that case).
    if (std::isnan(cfg.start_c) || std::isnan(cfg.full_c)) {
        throw std::runtime_error(
            channel_prefix + " " + stage +
            " requires both start_c and full_c");
    }
    ValidatePositive(cfg.rise_per_unit, channel_prefix + " " + rise_key, true);
    ValidatePositive(cfg.fall_per_unit, channel_prefix + " " + fall_key, true);
    ValidatePercentage(cfg.max_boost_pct, channel_prefix + " " + max_key, true);
    if (cfg.full_c <= cfg.start_c) {
        throw std::runtime_error(
            channel_prefix + " " + stage + "_full_c must be > start_c");
    }
}

// Loads a curve array (each element {temp_c, duty_pct}) into `out`, sorted by
// temp ascending. Matches the previous inline parse-and-sort pattern for the
// "curve" and "cpu_override_curve" channel fields.
void LoadCurveFromJson(const nlohmann::json& json,
                       const std::string& key,
                       std::vector<CurvePoint>& out) {
    if (!json.contains(key) || !json[key].is_array()) {
        return;
    }
    for (const auto& point_json : json[key]) {
        if (point_json.contains("temp_c") && point_json.contains("duty_pct")) {
            CurvePoint point;
            point.temp_c = point_json["temp_c"].get<double>();
            point.duty_pct = point_json["duty_pct"].get<double>();
            out.push_back(point);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const CurvePoint& a, const CurvePoint& b) {
                  return a.temp_c < b.temp_c;
              });
}

void ValidateChannelConfig(const ChannelControlConfig& ch,
                          std::uint32_t index) {
    const std::string prefix = "Channel " + std::to_string(ch.channel) +
                              " (index " + std::to_string(index) + ")";

    ValidatePercentage(ch.min_duty_pct, prefix + " min_duty_pct");
    ValidatePercentage(ch.deadband_pct, prefix + " deadband_pct", true);
    ValidatePositive(ch.source_aware_cpu_hot_guard_c,
                    prefix + " source_aware_cpu_hot_guard_c", true);

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

    for (std::size_t i = 0; i < kBoostStageCount; ++i) {
        ValidateBoostStageConfig(ch.boosts[i], kBoostStageSpecs[i], prefix);
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

    // FEAT-0003: malformed PID config fails the load. The pid.allow_live live/
    // shadow gate is NOT enforced here -- per decision D6 an unevidenced
    // allow_live is downgraded to shadow at controller construction, not a load
    // failure, so the rest of the channels still start.
    if (ch.controller_kind == ControllerKind::Pid) {
        if (std::isnan(ch.pid.target_c)) {
            throw std::runtime_error(
                prefix + " pid.target_c is required when controller is pid");
        }
        ValidatePositive(ch.pid.kp, prefix + " pid.kp");
        ValidatePositive(ch.pid.ki, prefix + " pid.ki");
        ValidatePositive(ch.pid.kd, prefix + " pid.kd");
        if (ch.pid.feedforward == PidFeedforward::Fixed) {
            ValidatePercentage(ch.pid.fixed_feedforward_pct,
                               prefix + " pid.fixed_feedforward_pct");
        }
        if (!std::isnan(ch.pid.integral_min) &&
            !std::isnan(ch.pid.integral_max) &&
            ch.pid.integral_min >= ch.pid.integral_max) {
            throw std::runtime_error(
                prefix + " pid.integral_min must be < pid.integral_max");
        }
    }
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

void ValidateCadenceConfig(const ControlLoopConfig& cfg) {
    // poll_tick_floor_ms == poll_tick_ms means adaptation is off (the
    // loader's default when the key is absent); skip the floor bounds so
    // configs without cadence fields are unaffected.
    if (cfg.poll_tick_floor_ms == cfg.poll_tick_ms) {
        return;
    }
    if (cfg.poll_tick_floor_ms == 0u) {
        throw std::runtime_error("poll_tick_floor_ms must be > 0");
    }
    if (cfg.poll_tick_floor_ms < 25u) {
        throw std::runtime_error(
            "poll_tick_floor_ms must be >= 25, got " +
            std::to_string(cfg.poll_tick_floor_ms));
    }
    if (cfg.poll_tick_floor_ms > cfg.poll_tick_ms) {
        throw std::runtime_error(
            "poll_tick_floor_ms must be <= poll_tick_ms (cadence may only "
            "tighten); got poll_tick_floor_ms=" +
            std::to_string(cfg.poll_tick_floor_ms) +
            " poll_tick_ms=" + std::to_string(cfg.poll_tick_ms));
    }
    ValidatePositive(cfg.cadence_slew_start_c_per_s,
                     "cadence_slew_start_c_per_s");
    ValidatePositive(cfg.cadence_slew_full_c_per_s,
                     "cadence_slew_full_c_per_s");
    if (cfg.cadence_slew_full_c_per_s <= cfg.cadence_slew_start_c_per_s) {
        throw std::runtime_error(
            "cadence_slew_full_c_per_s must be > "
            "cadence_slew_start_c_per_s");
    }
    ValidatePositive(cfg.cadence_relax_per_s, "cadence_relax_per_s");
}

// Optional low_band sub-object parser. No-op when the parent JSON has no
// low_band key or the value is not an object. All fields fall back to the
// LowBandControlConfig defaults that the caller pre-populates.
void LoadLowBandConfig(const nlohmann::json& loop_json,
                       LowBandControlConfig& low) {
    if (!loop_json.contains("low_band") ||
        !loop_json["low_band"].is_object()) {
        return;
    }
    const auto& low_json = loop_json["low_band"];
    low.enabled = low_json.value("enabled", low.enabled);
    low.cpu_start_c = low_json.value("cpu_start_c", low.cpu_start_c);
    low.cpu_full_c = low_json.value("cpu_full_c", low.cpu_full_c);
    low.cpu_release_c = low_json.value("cpu_release_c", low.cpu_release_c);
    low.gpu_start_c = low_json.value("gpu_start_c", low.gpu_start_c);
    low.gpu_full_c = low_json.value("gpu_full_c", low.gpu_full_c);
    low.gpu_release_c = low_json.value("gpu_release_c", low.gpu_release_c);
    low.cpu_weight = low_json.value("cpu_weight", low.cpu_weight);
    low.gpu_weight = low_json.value("gpu_weight", low.gpu_weight);
    low.rise_per_min = low_json.value("rise_per_min", low.rise_per_min);
    low.fall_per_min = low_json.value("fall_per_min", low.fall_per_min);
    low.stage_rise_pct_per_min =
        low_json.value("stage_rise_pct_per_min", low.stage_rise_pct_per_min);
    low.stage_fall_pct_per_min =
        low_json.value("stage_fall_pct_per_min", low.stage_fall_pct_per_min);
    low.stage_spacing_ms =
        low_json.value("stage_spacing_ms", low.stage_spacing_ms);
    low.evidence_write_interval_ms =
        low_json.value("evidence_write_interval_ms",
                       low.evidence_write_interval_ms);
}

// Optional pid sub-object parser (FEAT-0003). No-op when the channel JSON has
// no pid key or the value is not an object; fields fall back to the
// PidChannelConfig defaults the caller pre-populates. ParsePidFeedforward
// throws on an unknown feedforward string so a typo fails config load loudly.
void LoadPidConfig(const nlohmann::json& ch_json, PidChannelConfig& pid) {
    if (!ch_json.contains("pid") || !ch_json["pid"].is_object()) {
        return;
    }
    const auto& pid_json = ch_json["pid"];
    ReadOptionalDouble(pid_json, "target_c", pid.target_c);
    pid.kp = pid_json.value("kp", pid.kp);
    pid.ki = pid_json.value("ki", pid.ki);
    pid.kd = pid_json.value("kd", pid.kd);
    if (pid_json.contains("feedforward")) {
        pid.feedforward =
            ParsePidFeedforward(pid_json["feedforward"].get<std::string>());
    }
    ReadOptionalDouble(pid_json, "fixed_feedforward_pct",
                       pid.fixed_feedforward_pct);
    ReadOptionalDouble(pid_json, "integral_min", pid.integral_min);
    ReadOptionalDouble(pid_json, "integral_max", pid.integral_max);
    pid.allow_live = pid_json.value("allow_live", pid.allow_live);
    pid.characterization_artifact =
        pid_json.value("characterization_artifact",
                       pid.characterization_artifact);
}

// Builds one ChannelControlConfig from its JSON object. Assumes the caller
// has already verified the "channel" field is present; the curve fields
// use LoadCurveFromJson, and each boost overlay is parsed by
// LoadBoostStageConfig driven by kBoostStageSpecs.
ChannelControlConfig LoadChannelConfig(const nlohmann::json& ch_json) {
    ChannelControlConfig channel;
    channel.channel = ch_json["channel"].get<std::uint32_t>();

    channel.min_duty_pct = ch_json.value("min_duty_pct", channel.min_duty_pct);
    ReadOptionalDouble(ch_json, "source_aware_cpu_hot_guard_c",
                       channel.source_aware_cpu_hot_guard_c);
    channel.write_cooldown_ms =
        ch_json.value("write_cooldown_ms", channel.write_cooldown_ms);
    ReadOptionalDouble(ch_json, "deadband_pct", channel.deadband_pct);
    channel.control_hold_ms =
        ch_json.value("control_hold_ms", channel.control_hold_ms);

    // Parse curve shape. ParseCurveShape throws on unknown strings; let that
    // propagate so a typo fails config load loudly (consistent with the other
    // channel validators in this file).
    if (ch_json.contains("curve_shape")) {
        channel.curve_shape = ParseCurveShape(
            ch_json["curve_shape"].get<std::string>());
    }

    ReadOptionalDouble(ch_json, "rise_rate_pct_per_min",
                       channel.rise_rate_pct_per_min);
    ReadOptionalDouble(ch_json, "fall_rate_pct_per_min",
                       channel.fall_rate_pct_per_min);
    ReadOptionalDouble(ch_json, "max_setpoint_step_pct",
                       channel.max_setpoint_step_pct);

    ReadOptionalDouble(ch_json, "demand_smoothing_rise_alpha",
                       channel.demand_smoothing_rise_alpha);
    ReadOptionalDouble(ch_json, "demand_smoothing_fall_alpha",
                       channel.demand_smoothing_fall_alpha);

    ReadOptionalDouble(ch_json, "decay_latch_above_pct",
                       channel.decay_latch_above_pct);
    ReadOptionalDouble(ch_json, "decay_latch_pct_per_min",
                       channel.decay_latch_pct_per_min);

    for (std::size_t i = 0; i < kBoostStageCount; ++i) {
        LoadBoostStageConfig(ch_json, kBoostStageSpecs[i],
                             channel.boosts[i]);
    }

    channel.low_band_stage =
        ch_json.value("low_band_stage", channel.low_band_stage);
    ReadOptionalDouble(ch_json, "low_band_debt_threshold",
                       channel.low_band_debt_threshold);
    channel.low_band_hold_ms =
        ch_json.value("low_band_hold_ms", channel.low_band_hold_ms);
    ReadOptionalDouble(ch_json, "low_band_max_boost_pct",
                       channel.low_band_max_boost_pct);

    // Parse temp blend. ParseTempBlend throws on unknown strings; let that
    // propagate. Silent fallback to CpuOnly would defeat the GPU response
    // lane on a typo, which is harder to diagnose than a config rejection.
    if (ch_json.contains("temp_blend")) {
        channel.temp_blend = ParseTempBlend(
            ch_json["temp_blend"].get<std::string>());
    }

    // Control-law discriminator (FEAT-0003). ParseControllerKind throws on an
    // unknown string; an absent key keeps the curve_overlay default.
    if (ch_json.contains("controller")) {
        channel.controller_kind = ParseControllerKind(
            ch_json["controller"].get<std::string>());
    }
    LoadPidConfig(ch_json, channel.pid);

    LoadCurveFromJson(ch_json, "curve", channel.curve);
    LoadCurveFromJson(ch_json, "cpu_override_curve",
                      channel.cpu_override_curve);

    return channel;
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

    ValidateCadenceConfig(cfg);
    ValidatePercentage(cfg.deadband_pct, "deadband_pct");
    ValidatePercentage(cfg.low_band_residual_cap_pct,
                       "low_band_residual_cap_pct", true);
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
    // FEAT-0023 (REQ-MPROFILE-01): resolve a composition descriptor
    // (machine-base + behavior-overlay) to its merged control config, or the
    // file's own JSON unchanged when it is not a descriptor.
    const nlohmann::json root = ComposeConfigRoot(config_path).root;

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

    // Upward-only adaptive cadence. An absent poll_tick_floor_ms defaults to
    // poll_tick_ms, which disables adaptation; an absent cadence_relax_per_s
    // derives (P - F) / 3.
    cfg.poll_tick_floor_ms = loop_json.contains("poll_tick_floor_ms")
        ? loop_json.value("poll_tick_floor_ms", cfg.poll_tick_ms)
        : cfg.poll_tick_ms;
    cfg.cadence_slew_start_c_per_s = loop_json.value(
        "cadence_slew_start_c_per_s", cfg.cadence_slew_start_c_per_s);
    cfg.cadence_slew_full_c_per_s = loop_json.value(
        "cadence_slew_full_c_per_s", cfg.cadence_slew_full_c_per_s);
    cfg.cadence_relax_per_s = loop_json.contains("cadence_relax_per_s")
        ? loop_json.value("cadence_relax_per_s", 0.0)
        : (static_cast<double>(cfg.poll_tick_ms) -
           static_cast<double>(cfg.poll_tick_floor_ms)) / 3.0;

    ReadOptionalDouble(loop_json, "low_band_residual_cap_pct",
                       cfg.low_band_residual_cap_pct);
    LoadLowBandConfig(loop_json, cfg.low_band);

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
        cfg.channels.push_back(LoadChannelConfig(ch_json));
    }

    // Validate the loaded configuration
    ValidateControlLoopConfig(cfg, config_path);

    return cfg;
}

}  // namespace svg_mb_control
