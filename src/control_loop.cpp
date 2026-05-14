#include "control_loop.h"

#include "amd_reader.h"
#include "channel_evaluator.h"
#include "control_runtime_context.h"
#include "control_scheduler.h"
#include "control_status_writer.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "pending_writes.h"
#include "runtime_logging.h"
#include "runtime_snapshot.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

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
    ValidateCurve(ch.cpu_override_curve, prefix + " cpu_override_curve", true);
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

struct ControlLoop::Impl {
    Impl(ControlConfig base_config,
         ControlLoopConfig loop_config,
         std::filesystem::path runtime_home_path)
        : context(std::move(base_config),
                  std::move(loop_config),
                  std::move(runtime_home_path)) {}

    ControlRuntimeContext context;
};

ControlLoop::ControlLoop(ControlConfig base_config,
                         ControlLoopConfig loop_config,
                         std::filesystem::path runtime_home)
    : impl_(std::make_unique<Impl>(std::move(base_config),
                                   std::move(loop_config),
                                   std::move(runtime_home))) {}

ControlLoop::~ControlLoop() = default;

int ControlLoop::RunUntilStopped(const std::atomic<bool>& stop_flag) {
    ControlRuntimeContext& context = impl_->context;
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(context.runtime_home).string();
    RuntimeCsvLogger csv_logger(
        context.runtime_home,
        context.base.log_rotate_hours,
        context.base.log_retain_days);
    std::string log_csv_path;
    if (csv_logger.Open("control-loop", BuildControlLoopCsvHeader())) {
        log_csv_path = csv_logger.active_archive_path().string();
    }
    RuntimeControlLoopTimingState last_timing;
    last_timing.loop_intended_interval_ms = context.loop.poll_tick_ms;

    AmdReader amd_reader;
    GpuReader gpu_reader;

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(context.runtime_policy);
    } catch (const std::exception& error) {
        AppendRuntimeEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.init_failed",
                .detail = std::string("direct writer init failed: ") +
                          error.what(),
                .success = false,
            });
        WriteControlLoopStatus(context.runtime_home, "control-loop", "failed",
                        std::string("direct writer init failed: ") + error.what(),
                        0u, FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, context.channels, log_csv_path, event_log_path);
        return 1;
    }

    TimerResolutionScope timer_resolution(1u);

    std::uint64_t tick_count = 0u;
    {
        std::ostringstream detail;
        detail << "direct telemetry active; fan_writer="
               << fan_writer->BackendLabel()
               << " policy="
               << (context.runtime_policy.present
                       ? context.runtime_policy.source_path.string()
                       : std::string("(none)"))
               << " poll_tick_ms="
               << context.loop.poll_tick_ms
               << " cooldown_ms=" << context.loop.write_cooldown_ms
               << " deadband_pct=" << context.loop.deadband_pct
               << " hold_ms=" << context.loop.control_hold_ms
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"))
               << " channels=" << context.channels.size() << ")";
        WriteControlLoopStatus(context.runtime_home, "control-loop", "running",
                        detail.str(), tick_count,
                        FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, context.channels, log_csv_path, event_log_path);
    }
    AppendRuntimeEvent(
        context.runtime_home,
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
            amd_reader, gpu_reader, *fan_writer, context.runtime_policy);
        const bool runtime_snapshot_available =
            RuntimeSnapshotHasTelemetry(runtime_snapshot);

        if (csv_logger.MaybeRotate()) {
            log_csv_path = csv_logger.active_archive_path().string();
            AppendRuntimeEvent(
                context.runtime_home,
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
                runtime_snapshot, context.loop.cpu_temp_label);
            if (!std::isnan(cpu_c)) {
                temp_inputs.cpu_c = cpu_c;
                temp_inputs.cpu_available = true;
                temp_inputs.cpu_label = context.loop.cpu_temp_label;
            }
        }
        if (runtime_snapshot.gpu.available) {
            temp_inputs.gpu_c = GpuControlEnvelopeC(runtime_snapshot.gpu);
            temp_inputs.gpu_available = true;
            temp_inputs.gpu_label = "gpu_envelope";
        }

        if (runtime_snapshot_available) {
            WriteRuntimeSnapshotFile(context.runtime_home, runtime_snapshot);
        }

        // Per-channel decisions.
        for (auto& channel : context.channels) {
            const ChannelTimingConfig channel_timing =
                BuildChannelTimingConfig(context.loop, channel, now_steady);

            if (!channel.baseline_captured) {
                if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                        runtime_snapshot, channel.config.channel)) {
                    channel.baseline_duty_raw = fan->duty_raw;
                    channel.baseline_mode_raw = fan->mode_raw;
                    channel.baseline_captured = true;
                    AppendRuntimeEvent(
                        context.runtime_home,
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
                channel_timing.effective_hold_ms > 0u &&
                now_steady >= channel.hold_deadline &&
                channel.baseline_captured) {
                const FanWriteResult restore_result =
                    fan_writer->RestoreSavedState(channel.config.channel,
                                                  channel.baseline_duty_raw,
                                                  channel.baseline_mode_raw,
                                                  context.base.restore_timeout_ms);
                if (restore_result) {
                    AppendRuntimeEvent(
                        context.runtime_home,
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
                        RemovePendingWrite(context.runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but don't fail
                        AppendRuntimeEvent(
                            context.runtime_home,
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
                        context.runtime_home,
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

            const ChannelEvaluation evaluation = EvaluateChannel(
                channel, context.loop, temp_inputs, runtime_snapshot, now_steady);
            if (evaluation.sensor_event ==
                ChannelSensorEvent::FailureDetected) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sensor_failure_detected",
                        .detail = "sensor failure detected, entering safe mode (100% duty)",
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
            } else if (evaluation.sensor_event ==
                       ChannelSensorEvent::Recovered) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sensor_recovered",
                        .detail = "sensor recovered, resuming normal control",
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c =
                            evaluation.sensor_event_observed_temp_c,
                        .success = true,
                    });
            }
            if (!evaluation.has_setpoint) {
                continue;
            }

            const double observed_temp_c = evaluation.observed_temp_c;
            const double setpoint = evaluation.setpoint_pct;

            if (!std::isnan(channel.last_issued_pct)) {
                const double delta = std::abs(setpoint - channel.last_issued_pct);
                if (!evaluation.authority_reassert &&
                    delta < evaluation.timing.effective_deadband_pct) {
                    continue;
                }
            }

            if (std::isnan(channel.last_issued_pct)) {
                // First write for this channel — allow immediately.
            } else if (evaluation.timing.elapsed_since_last_write_ms <
                       WriteCooldownForAuthorityReassert(
                           evaluation.timing,
                           evaluation.authority_reassert)) {
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
            entry.requested_hold_ms = evaluation.timing.effective_hold_ms;
            entry.started_iso = eval_iso;
            entry.child_pid = 0u;
            try {
                UpsertPendingWrite(context.runtime_home, entry);
            } catch (const std::exception& e) {
                AppendRuntimeEvent(
                    context.runtime_home,
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
                            context.runtime_home,
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
                        RemovePendingWrite(context.runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but continue
                        AppendRuntimeEvent(
                            context.runtime_home,
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
                    context.runtime_home,
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
                        context.runtime_home,
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
            if (evaluation.authority_reassert) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.authority_reasserted",
                        .detail = evaluation.authority_detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c = observed_temp_c,
                        .setpoint_pct = setpoint,
                        .success = true,
                    });
            }
            channel.write_active = true;
            channel.hold_deadline =
                evaluation.timing.effective_hold_ms == 0u
                    ? std::chrono::steady_clock::time_point::max()
                    : now_steady + std::chrono::milliseconds(
                          evaluation.timing.effective_hold_ms);
            channel.last_issued_pct = setpoint;
            channel.last_write_time = now_steady;
            ++channel.total_writes;
            AppendRuntimeEvent(
                context.runtime_home,
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
        tick_timing.loop_intended_interval_ms = context.loop.poll_tick_ms;
        tick_timing.loop_achieved_interval_ms = achieved_interval_ms;
        tick_timing.loop_slip_ms = std::isnan(achieved_interval_ms)
            ? std::numeric_limits<double>::quiet_NaN()
            : achieved_interval_ms -
                  static_cast<double>(context.loop.poll_tick_ms);
        tick_timing.loop_overrun =
            work_duration_ms > static_cast<double>(context.loop.poll_tick_ms);
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
                context.runtime_home,
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
                    BuildChannelLogStates(context.channels)));
        }

        // Rate-limit status file writes to reduce disk I/O. CSV remains per tick.
        // Write every 10 ticks (500 ms with the shipped 50 ms cadence).
        constexpr std::uint32_t kStatusUpdateIntervalTicks = 10u;
        const bool should_write_status = (tick_count % kStatusUpdateIntervalTicks) == 0u;

        if (should_write_status) {
            std::ostringstream td;
            td << "tick poll_tick_ms=" << context.loop.poll_tick_ms
               << " cooldown=" << context.loop.write_cooldown_ms
               << " deadband=" << context.loop.deadband_pct
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"));
            WriteControlLoopStatus(context.runtime_home, "control-loop", "running",
                            td.str(), tick_count, eval_iso, last_timing,
                            context.channels, log_csv_path, event_log_path);
        }

        WaitForNextControlTick(context, tick_started_steady, stop_flag);
    }

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    "stop requested", tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, context.channels, log_csv_path, event_log_path);
    AppendRuntimeEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.shutdown_requested",
            .detail = "stop requested",
            .tick_count = tick_count,
            .success = true,
        });
    bool restore_failure = false;
    for (auto& channel : context.channels) {
        if (fatal_restore_timeout && channel.write_active) {
            restore_failure = true;
            AppendRuntimeEvent(
                context.runtime_home,
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
                                              context.base.restore_timeout_ms);
            if (!restore_result) {
                restore_failure = true;
                AppendRuntimeEvent(
                    context.runtime_home,
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
                context.runtime_home,
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
            RemovePendingWrite(context.runtime_home, channel.config.channel);
        } catch (const std::exception& e) {
            restore_failure = true;
            AppendRuntimeEvent(
                context.runtime_home,
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
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    restore_failure ? "restore failed"
                                    : "channels restored",
                    tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, context.channels, log_csv_path, event_log_path);
    AppendRuntimeEvent(
        context.runtime_home,
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
