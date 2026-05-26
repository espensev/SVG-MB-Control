#include "low_band_evidence.h"

#include "json_io.h"
#include "runtime_artifacts.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace svg_mb_control {

namespace {

double MeanOrNan(double sum, std::uint64_t count) {
    return count > 0u
        ? sum / static_cast<double>(count)
        : std::numeric_limits<double>::quiet_NaN();
}

nlohmann::json NanToNull(double value) {
    return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json(nullptr);
}

}  // namespace

bool LowBandChannelConfigured(const ChannelState& channel) {
    return channel.config.low_band_stage > 0u &&
        !std::isnan(channel.config.low_band_debt_threshold) &&
        !std::isnan(channel.config.low_band_max_boost_pct) &&
        channel.config.low_band_max_boost_pct > 0.0;
}

void WriteLowBandEvidenceFile(const ControlRuntimeContext& context,
                              std::uint64_t tick_count) {
    const LowBandRuntimeState& state = context.low_band;
    const LowBandControlConfig& cfg = context.loop.low_band;
    nlohmann::json payload;
    payload["schema"] = "svg_mb_control.low_band_evidence.v1";
    payload["schema_version"] = 1;
    payload["captured_local"] =
        FormatRuntimeLocalIso8601(std::chrono::system_clock::now());
    payload["tick_count"] = tick_count;
    payload["enabled"] = cfg.enabled;
    payload["debt"] = state.debt;
    payload["signal"] = state.signal;
    payload["cpu_scale"] = state.cpu_scale;
    payload["gpu_scale"] = state.gpu_scale;
    payload["sample_count"] = state.sample_count;
    payload["active_sample_count"] = state.active_sample_count;
    payload["max_debt"] = state.max_debt;
    payload["max_cpu_c"] = NanToNull(state.max_cpu_c);
    payload["max_gpu_c"] = NanToNull(state.max_gpu_c);
    payload["config"] = {
        {"cpu_start_c", cfg.cpu_start_c},
        {"cpu_full_c", cfg.cpu_full_c},
        {"cpu_release_c", cfg.cpu_release_c},
        {"gpu_start_c", cfg.gpu_start_c},
        {"gpu_full_c", cfg.gpu_full_c},
        {"gpu_release_c", cfg.gpu_release_c},
        {"cpu_weight", cfg.cpu_weight},
        {"gpu_weight", cfg.gpu_weight},
        {"rise_per_min", cfg.rise_per_min},
        {"fall_per_min", cfg.fall_per_min},
        {"stage_rise_pct_per_min", cfg.stage_rise_pct_per_min},
        {"stage_fall_pct_per_min", cfg.stage_fall_pct_per_min},
        {"stage_spacing_ms", cfg.stage_spacing_ms},
        {"evidence_write_interval_ms", cfg.evidence_write_interval_ms},
    };

    payload["channels"] = nlohmann::json::array();
    for (const auto& channel : context.channels) {
        const double baseline_rpm = MeanOrNan(
            channel.low_band_unboosted_rpm_sum,
            channel.low_band_unboosted_rpm_count);
        const double boosted_rpm = MeanOrNan(
            channel.low_band_boosted_rpm_sum,
            channel.low_band_boosted_rpm_count);
        nlohmann::json channel_json = {
            {"channel", channel.config.channel},
            {"configured", LowBandChannelConfigured(channel)},
            {"stage", channel.config.low_band_stage},
            {"debt_threshold", NanToNull(channel.config.low_band_debt_threshold)},
            {"hold_ms", channel.config.low_band_hold_ms},
            {"max_boost_pct_config",
             NanToNull(channel.config.low_band_max_boost_pct)},
            {"stage_active", channel.low_band_stage_active},
            {"eligible_ms", channel.low_band_eligible_ms},
            {"activation_count", channel.low_band_activation_count},
            {"current_boost_pct", channel.low_band_stage_boost_pct},
            {"max_observed_boost_pct", channel.low_band_max_boost_pct},
            {"sample_count", channel.low_band_sample_count},
            {"active_sample_count", channel.low_band_active_sample_count},
            {"boost_area_pct_s", channel.low_band_boost_area_pct_s},
            {"rpm_unboosted_mean", NanToNull(baseline_rpm)},
            {"rpm_boosted_mean", NanToNull(boosted_rpm)},
            {"rpm_delta_mean", NanToNull(
                 std::isnan(baseline_rpm) || std::isnan(boosted_rpm)
                     ? std::numeric_limits<double>::quiet_NaN()
                     : boosted_rpm - baseline_rpm)},
            {"total_writes", channel.total_writes},
        };
        payload["channels"].push_back(std::move(channel_json));
    }

    if (!TryWriteJsonFileAtomic(context.runtime_home / "low_band_evidence.json",
                                payload)) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.low_band_evidence_write_failed",
                .detail = "failed to write low_band_evidence.json",
                .tick_count = tick_count,
                .success = false,
            });
    }
}

}  // namespace svg_mb_control
