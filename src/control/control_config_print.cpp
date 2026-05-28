#include "control_config_print.h"

#include "control_policy.h"
#include "json_io.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace svg_mb_control {

namespace {

bool IsKnown(double value) {
    return !std::isnan(value);
}

std::string FormatNumber(double value, int precision) {
    if (!IsKnown(value)) {
        return "(unset)";
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string FormatPctRange(double start_c, double full_c, double max_boost_pct) {
    if (!IsKnown(start_c) && !IsKnown(full_c) && !IsKnown(max_boost_pct)) {
        return "disabled";
    }
    std::ostringstream oss;
    oss << FormatNumber(start_c, 1) << "->" << FormatNumber(full_c, 1)
        << " C, max +" << FormatNumber(max_boost_pct, 1) << "%";
    return oss.str();
}

void PrintCurveSummary(std::ostream& out, const std::vector<CurvePoint>& curve,
                       const char* label) {
    if (curve.empty()) {
        out << "       " << label << "  (no points)\n";
        return;
    }
    const auto& first = curve.front();
    const auto& last = curve.back();
    out << "       " << label << "  " << FormatNumber(first.temp_c, 1)
        << " C / " << FormatNumber(first.duty_pct, 1) << "% .. "
        << FormatNumber(last.temp_c, 1) << " C / "
        << FormatNumber(last.duty_pct, 1) << "%  (" << curve.size()
        << " pts)\n";
}

// Descriptor for the three pressure-style boost blocks that share the same
// (start_c, full_c, rise_pct_per_sec, fall_pct_per_sec, max_boost_pct) shape:
// Short text labels for the show-config text summary. JSON keys come from
// kBoostStageSpecs[i].name. cpu_low_soak stays bespoke below because it
// uses per_min rates and an explicit release_c.
struct PressureBoostTextLabel {
    const char* text_label;
    BoostStage stage;
};

constexpr PressureBoostTextLabel kPressureBoostTextLabels[] = {
    {"thermal", BoostStage::ThermalPressure},
    {"midband", BoostStage::MidbandPressure},
    {"gpu-air", BoostStage::GpuAirflow},
};

void EmitPressureBoostText(std::ostream& out,
                           const ChannelControlConfig& ch,
                           const PressureBoostTextLabel& label) {
    const auto& cfg =
        ch.boosts[static_cast<std::size_t>(label.stage)];
    out << "       " << label.text_label << " "
        << FormatPctRange(cfg.start_c, cfg.full_c, cfg.max_boost_pct)
        << '\n';
}

void PrintTextSummary(std::ostream& out, const ControlConfig& base,
                      const std::optional<ControlLoopConfig>& loop) {
    out << "svg-mb-control config: ";
    if (base.source_path.empty()) {
        out << "(none)";
    } else {
        out << base.source_path.string();
    }
    out << "\n\n";

    out << "Schema version: " << base.schema_version << '\n';
    if (!base.default_mode.empty()) {
        out << "Default mode:   " << base.default_mode << '\n';
    }
    if (!base.runtime_home_path.empty()) {
        out << "Runtime home:   " << base.runtime_home_path.string() << '\n';
    }
    if (!base.runtime_policy_path.empty()) {
        out << "Runtime policy: " << base.runtime_policy_path.string() << '\n';
    }
    out << '\n';

    out << "Loop cadence:\n";
    out << "  read-loop poll          " << base.poll_ms << " ms\n";
    if (loop.has_value()) {
        out << "  control-loop tick       " << loop->poll_tick_ms << " ms";
        if (loop->poll_tick_floor_ms == 0u ||
            loop->poll_tick_floor_ms == loop->poll_tick_ms) {
            out << "  (adaptive floor: disabled)\n";
        } else {
            out << "  (adaptive floor: " << loop->poll_tick_floor_ms
                << " ms)\n";
        }
        out << "  cadence slew band       "
            << FormatNumber(loop->cadence_slew_start_c_per_s, 2) << " -> "
            << FormatNumber(loop->cadence_slew_full_c_per_s, 2) << " C/s\n";
        if (loop->cadence_relax_per_s > 0.0) {
            out << "  cadence relax           "
                << FormatNumber(loop->cadence_relax_per_s, 3) << " /s\n";
        }
    }
    out << '\n';

    if (loop.has_value()) {
        out << "Write timing (loop defaults):\n";
        out << "  write cooldown          " << loop->write_cooldown_ms << " ms\n";
        out << "  control hold            " << loop->control_hold_ms << " ms\n";
        out << "  deadband                " << FormatNumber(loop->deadband_pct, 2)
            << " %\n";
        out << '\n';
    }

    out << "Health/safety:\n";
    if (base.staleness_threshold_ms > 0u) {
        out << "  staleness threshold     " << base.staleness_threshold_ms
            << " ms\n";
    } else {
        out << "  staleness threshold     (default 10000 ms)\n";
    }
    out << "  restore timeout         " << base.restore_timeout_ms << " ms\n";
    out << "  baseline freshness      " << base.baseline_freshness_ceiling_ms
        << " ms ceiling\n";
    out << "  log rotate / retain     " << base.log_rotate_hours << " h / "
        << base.log_retain_days << " d\n";
    out << "  csv flush interval      " << base.csv_flush_interval_rows
        << " rows\n";
    out << '\n';

    if (loop.has_value() && loop->low_band.enabled) {
        const auto& lb = loop->low_band;
        out << "Low-band global:\n";
        out << "  enabled                 true\n";
        out << "  cpu band                " << FormatNumber(lb.cpu_start_c, 1)
            << " -> " << FormatNumber(lb.cpu_full_c, 1) << " C  (release "
            << FormatNumber(lb.cpu_release_c, 1) << ")\n";
        out << "  gpu band                " << FormatNumber(lb.gpu_start_c, 1)
            << " -> " << FormatNumber(lb.gpu_full_c, 1) << " C  (release "
            << FormatNumber(lb.gpu_release_c, 1) << ")\n";
        out << "  cpu/gpu weight          " << FormatNumber(lb.cpu_weight, 2)
            << " / " << FormatNumber(lb.gpu_weight, 2) << '\n';
        out << "  debt rise/fall          " << FormatNumber(lb.rise_per_min, 3)
            << " / " << FormatNumber(lb.fall_per_min, 3) << " per min\n";
        out << "  stage rise/fall         "
            << FormatNumber(lb.stage_rise_pct_per_min, 2) << " / "
            << FormatNumber(lb.stage_fall_pct_per_min, 2) << " %/min\n";
        out << "  stage spacing           " << lb.stage_spacing_ms << " ms\n";
        if (IsKnown(loop->low_band_residual_cap_pct)) {
            out << "  residual cap            "
                << FormatNumber(loop->low_band_residual_cap_pct, 2) << " %\n";
        } else {
            out << "  residual cap            (disabled)\n";
        }
        out << '\n';
    } else if (loop.has_value()) {
        out << "Low-band global:          disabled\n\n";
    }

    if (loop.has_value()) {
        out << "Channels (" << loop->channels.size() << "):\n";
        for (const auto& ch : loop->channels) {
            out << "  ch" << ch.channel << "  blend=" << TempBlendToString(ch.temp_blend)
                << "  shape=" << CurveShapeToString(ch.curve_shape)
                << "  min=" << FormatNumber(ch.min_duty_pct, 1) << "%";
            if (IsKnown(ch.source_aware_cpu_hot_guard_c)) {
                out << "  source_guard="
                    << FormatNumber(ch.source_aware_cpu_hot_guard_c, 1)
                    << " C";
            }
            if (ch.low_band_stage > 0u) {
                out << "  low_band_stage=" << ch.low_band_stage;
            }
            out << '\n';

            out << "       rate    rise " << FormatNumber(ch.rise_rate_pct_per_min, 1)
                << " / fall " << FormatNumber(ch.fall_rate_pct_per_min, 1)
                << " %/min,  step<=" << FormatNumber(ch.max_setpoint_step_pct, 2)
                << "%\n";
            out << "       smooth  alpha rise "
                << FormatNumber(ch.demand_smoothing_rise_alpha, 4)
                << ",  alpha fall "
                << FormatNumber(ch.demand_smoothing_fall_alpha, 4);
            if (IsKnown(ch.decay_latch_pct_per_min)) {
                out << ",  decay latch "
                    << FormatNumber(ch.decay_latch_pct_per_min, 1) << " %/min above "
                    << FormatNumber(ch.decay_latch_above_pct, 1) << "%";
            }
            out << '\n';
            for (const auto& label : kPressureBoostTextLabels) {
                EmitPressureBoostText(out, ch, label);
            }
            const auto& soak = ch.boosts[
                static_cast<std::size_t>(BoostStage::CpuLowSoak)];
            if (IsKnown(soak.start_c) || IsKnown(soak.full_c) ||
                IsKnown(soak.max_boost_pct)) {
                out << "       cpu-soak "
                    << FormatPctRange(soak.start_c, soak.full_c,
                                      soak.max_boost_pct);
                if (IsKnown(soak.release_c)) {
                    out << " (release " << FormatNumber(soak.release_c, 1)
                        << " C)";
                }
                out << '\n';
            }
            PrintCurveSummary(out, ch.curve, "curve  ");
            if (!ch.cpu_override_curve.empty()) {
                PrintCurveSummary(out, ch.cpu_override_curve, "cpu-ovr");
            }
            out << '\n';
        }
    } else {
        out << "Channels: (no control_loop subtree)\n";
    }
}

nlohmann::json CurveToJson(const std::vector<CurvePoint>& curve) {
    auto array = nlohmann::json::array();
    for (const auto& point : curve) {
        array.push_back({{"temp_c", point.temp_c},
                         {"duty_pct", point.duty_pct}});
    }
    return array;
}

nlohmann::json OptionalDouble(double value) {
    if (!IsKnown(value)) {
        return nullptr;
    }
    return value;
}

nlohmann::json BuildPressureBoostJson(const BoostStageConfig& cfg) {
    return {
        {"start_c", OptionalDouble(cfg.start_c)},
        {"full_c", OptionalDouble(cfg.full_c)},
        {"rise_pct_per_sec", OptionalDouble(cfg.rise_per_unit)},
        {"fall_pct_per_sec", OptionalDouble(cfg.fall_per_unit)},
        {"max_boost_pct", OptionalDouble(cfg.max_boost_pct)},
    };
}

nlohmann::json BuildJsonSummary(const ControlConfig& base,
                                const std::optional<ControlLoopConfig>& loop) {
    nlohmann::json out;
    out["source_path"] = base.source_path.string();
    out["schema_version"] = base.schema_version;
    out["default_mode"] = base.default_mode;
    out["runtime_home_path"] = base.runtime_home_path.string();
    out["runtime_policy_path"] = base.runtime_policy_path.string();
    out["poll_ms"] = base.poll_ms;
    out["staleness_threshold_ms"] = base.staleness_threshold_ms;
    out["log_rotate_hours"] = base.log_rotate_hours;
    out["log_retain_days"] = base.log_retain_days;
    out["csv_flush_interval_rows"] = base.csv_flush_interval_rows;
    out["baseline_freshness_ceiling_ms"] = base.baseline_freshness_ceiling_ms;
    out["restore_timeout_ms"] = base.restore_timeout_ms;
    out["evidence_gpu_sample_mode"] = base.evidence_gpu_sample_mode;

    if (!loop.has_value()) {
        out["control_loop"] = nullptr;
        return out;
    }

    nlohmann::json loop_json;
    loop_json["poll_tick_ms"] = loop->poll_tick_ms;
    loop_json["poll_tick_floor_ms"] = loop->poll_tick_floor_ms;
    loop_json["write_cooldown_ms"] = loop->write_cooldown_ms;
    loop_json["control_hold_ms"] = loop->control_hold_ms;
    loop_json["deadband_pct"] = loop->deadband_pct;
    loop_json["cadence_slew_start_c_per_s"] = loop->cadence_slew_start_c_per_s;
    loop_json["cadence_slew_full_c_per_s"] = loop->cadence_slew_full_c_per_s;
    loop_json["cadence_relax_per_s"] = loop->cadence_relax_per_s;
    loop_json["low_band_residual_cap_pct"] =
        OptionalDouble(loop->low_band_residual_cap_pct);
    loop_json["cpu_temp_label"] = loop->cpu_temp_label;

    const auto& lb = loop->low_band;
    loop_json["low_band"] = {
        {"enabled", lb.enabled},
        {"cpu_start_c", lb.cpu_start_c},
        {"cpu_full_c", lb.cpu_full_c},
        {"cpu_release_c", lb.cpu_release_c},
        {"gpu_start_c", lb.gpu_start_c},
        {"gpu_full_c", lb.gpu_full_c},
        {"gpu_release_c", lb.gpu_release_c},
        {"cpu_weight", lb.cpu_weight},
        {"gpu_weight", lb.gpu_weight},
        {"rise_per_min", lb.rise_per_min},
        {"fall_per_min", lb.fall_per_min},
        {"stage_rise_pct_per_min", lb.stage_rise_pct_per_min},
        {"stage_fall_pct_per_min", lb.stage_fall_pct_per_min},
        {"stage_spacing_ms", lb.stage_spacing_ms},
        {"evidence_write_interval_ms", lb.evidence_write_interval_ms},
    };

    auto channels = nlohmann::json::array();
    for (const auto& ch : loop->channels) {
        nlohmann::json channel_json;
        channel_json["channel"] = ch.channel;
        channel_json["temp_blend"] = TempBlendToString(ch.temp_blend);
        channel_json["source_aware_cpu_hot_guard_c"] =
            OptionalDouble(ch.source_aware_cpu_hot_guard_c);
        channel_json["curve_shape"] = CurveShapeToString(ch.curve_shape);
        channel_json["min_duty_pct"] = ch.min_duty_pct;
        channel_json["write_cooldown_ms"] = ch.write_cooldown_ms;
        channel_json["control_hold_ms"] = ch.control_hold_ms;
        channel_json["deadband_pct"] = OptionalDouble(ch.deadband_pct);
        channel_json["rise_rate_pct_per_min"] =
            OptionalDouble(ch.rise_rate_pct_per_min);
        channel_json["fall_rate_pct_per_min"] =
            OptionalDouble(ch.fall_rate_pct_per_min);
        channel_json["max_setpoint_step_pct"] =
            OptionalDouble(ch.max_setpoint_step_pct);
        channel_json["demand_smoothing_rise_alpha"] =
            OptionalDouble(ch.demand_smoothing_rise_alpha);
        channel_json["demand_smoothing_fall_alpha"] =
            OptionalDouble(ch.demand_smoothing_fall_alpha);
        channel_json["decay_latch_above_pct"] =
            OptionalDouble(ch.decay_latch_above_pct);
        channel_json["decay_latch_pct_per_min"] =
            OptionalDouble(ch.decay_latch_pct_per_min);
        // Pressure-shaped stages (BelowStart + per_sec). JSON keys come
        // from kBoostStageSpecs[i].name.
        for (std::size_t i = 0; i < kBoostStageCount; ++i) {
            const auto& spec = kBoostStageSpecs[i];
            if (spec.release_mode != BoostReleaseMode::BelowStart) {
                continue;
            }
            channel_json[std::string(spec.name)] =
                BuildPressureBoostJson(ch.boosts[i]);
        }
        const auto& soak = ch.boosts[
            static_cast<std::size_t>(BoostStage::CpuLowSoak)];
        channel_json["cpu_low_soak"] = {
            {"start_c", OptionalDouble(soak.start_c)},
            {"full_c", OptionalDouble(soak.full_c)},
            {"release_c", OptionalDouble(soak.release_c)},
            {"rise_pct_per_min", OptionalDouble(soak.rise_per_unit)},
            {"fall_pct_per_min", OptionalDouble(soak.fall_per_unit)},
            {"max_boost_pct", OptionalDouble(soak.max_boost_pct)},
        };
        channel_json["low_band"] = {
            {"stage", ch.low_band_stage},
            {"debt_threshold", OptionalDouble(ch.low_band_debt_threshold)},
            {"hold_ms", ch.low_band_hold_ms},
            {"max_boost_pct", OptionalDouble(ch.low_band_max_boost_pct)},
        };
        channel_json["curve"] = CurveToJson(ch.curve);
        channel_json["cpu_override_curve"] = CurveToJson(ch.cpu_override_curve);
        channels.push_back(std::move(channel_json));
    }
    loop_json["channels"] = std::move(channels);
    out["control_loop"] = std::move(loop_json);
    return out;
}

}  // namespace

void PrintControlConfigSummary(std::ostream& out, const ControlConfig& base,
                               const std::optional<ControlLoopConfig>& loop,
                               bool json) {
    if (json) {
        out << BuildJsonSummary(base, loop).dump(2) << '\n';
        return;
    }
    PrintTextSummary(out, base, loop);
}

}  // namespace svg_mb_control
