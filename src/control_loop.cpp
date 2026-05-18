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
#include "runtime_artifacts.h"
#include "runtime_logging.h"
#include "runtime_lifecycle.h"
#include "runtime_snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace svg_mb_control {

// Configuration parsing/validation (LoadControlLoopConfig and its validators)
// lives in control_loop_config.cpp. This translation unit is the steady-state
// control loop engine only.

namespace {

double SmoothStep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * ((6.0 * t - 15.0) * t + 10.0);
}

double SmoothScale(double value, double start, double full) {
    if (std::isnan(value) || full <= start) {
        return 0.0;
    }
    return SmoothStep((value - start) / (full - start));
}

double MoveTowardRateLimited(double current,
                             double target,
                             double dt_minutes,
                             double rise_per_min,
                             double fall_per_min) {
    current = std::isnan(current) ? 0.0 : current;
    const double delta = target - current;
    if (std::abs(delta) <= 0.0001 || dt_minutes <= 0.0) {
        return target;
    }

    const double rate = delta > 0.0 ? rise_per_min : fall_per_min;
    if (std::isnan(rate) || rate <= 0.0) {
        return target;
    }

    const double allowed = rate * dt_minutes;
    if (std::abs(delta) <= allowed) {
        return target;
    }
    return current + (delta > 0.0 ? allowed : -allowed);
}

bool LowBandChannelConfigured(const ChannelState& channel) {
    return channel.config.low_band_stage > 0u &&
        !std::isnan(channel.config.low_band_debt_threshold) &&
        !std::isnan(channel.config.low_band_max_boost_pct) &&
        channel.config.low_band_max_boost_pct > 0.0;
}

double MeanOrNan(double sum, std::uint64_t count) {
    return count > 0u
        ? sum / static_cast<double>(count)
        : std::numeric_limits<double>::quiet_NaN();
}

nlohmann::json NanToNull(double value) {
    return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json(nullptr);
}

void UpdateLowBandState(ControlRuntimeContext& context,
                        const TempInputs& temp_inputs,
                        const RuntimeSnapshot& runtime_snapshot,
                        std::uint64_t elapsed_ms,
                        std::chrono::steady_clock::time_point now,
                        std::uint64_t tick_count) {
    LowBandRuntimeState& state = context.low_band;
    const LowBandControlConfig& cfg = context.loop.low_band;
    state.enabled = cfg.enabled;
    if (!cfg.enabled) {
        return;
    }

    if (elapsed_ms == 0u) {
        elapsed_ms = context.loop.poll_tick_ms;
    }
    const double dt_minutes = static_cast<double>(elapsed_ms) / 60000.0;
    const double dt_seconds = static_cast<double>(elapsed_ms) / 1000.0;

    const double cpu_scale = temp_inputs.cpu_available
        ? SmoothScale(temp_inputs.cpu_c, cfg.cpu_start_c, cfg.cpu_full_c)
        : 0.0;
    const double gpu_scale = temp_inputs.gpu_available
        ? SmoothScale(temp_inputs.gpu_c, cfg.gpu_start_c, cfg.gpu_full_c)
        : 0.0;
    const double signal = std::clamp(
        (std::max)(cfg.cpu_weight * cpu_scale, cfg.gpu_weight * gpu_scale),
        0.0, 1.0);

    const bool cpu_released =
        !temp_inputs.cpu_available || temp_inputs.cpu_c <= cfg.cpu_release_c;
    const bool gpu_released =
        !temp_inputs.gpu_available || temp_inputs.gpu_c <= cfg.gpu_release_c;

    if (signal > 0.0001) {
        state.debt += cfg.rise_per_min * signal * dt_minutes;
    } else if (cpu_released && gpu_released) {
        state.debt -= cfg.fall_per_min * dt_minutes;
    }
    state.debt = std::clamp(state.debt, 0.0, 1.0);
    state.signal = signal;
    state.cpu_scale = cpu_scale;
    state.gpu_scale = gpu_scale;
    ++state.sample_count;
    if (state.debt > 0.0001) {
        ++state.active_sample_count;
    }
    state.max_debt = (std::max)(state.max_debt, state.debt);
    if (temp_inputs.cpu_available) {
        state.max_cpu_c = std::isnan(state.max_cpu_c)
            ? temp_inputs.cpu_c
            : (std::max)(state.max_cpu_c, temp_inputs.cpu_c);
    }
    if (temp_inputs.gpu_available) {
        state.max_gpu_c = std::isnan(state.max_gpu_c)
            ? temp_inputs.gpu_c
            : (std::max)(state.max_gpu_c, temp_inputs.gpu_c);
    }

    const auto stage_spacing =
        std::chrono::milliseconds(cfg.stage_spacing_ms);

    for (auto& channel : context.channels) {
        channel.low_band_debt_snapshot = state.debt;
        channel.low_band_signal_snapshot = signal;
        channel.low_band_cpu_scale_snapshot = cpu_scale;
        channel.low_band_gpu_scale_snapshot = gpu_scale;
        ++channel.low_band_sample_count;

        const bool configured = LowBandChannelConfigured(channel);
        double target_boost_pct = 0.0;
        if (configured) {
            const double threshold = channel.config.low_band_debt_threshold;
            if (state.debt >= threshold) {
                channel.low_band_eligible_ms += elapsed_ms;
            } else {
                channel.low_band_eligible_ms = 0u;
                if (state.debt <= threshold * 0.75) {
                    channel.low_band_stage_active = false;
                }
            }

            const bool spacing_ok =
                !state.have_last_stage_activation ||
                (now - state.last_stage_activation_time) >= stage_spacing;
            if (!channel.low_band_stage_active &&
                channel.low_band_eligible_ms >=
                    channel.config.low_band_hold_ms &&
                spacing_ok) {
                channel.low_band_stage_active = true;
                ++channel.low_band_activation_count;
                state.have_last_stage_activation = true;
                state.last_stage_activation_time = now;
                std::ostringstream detail;
                detail << "low-band stage activated stage="
                       << channel.config.low_band_stage
                       << " debt=" << state.debt
                       << " threshold=" << threshold
                       << " cap_pct="
                       << channel.config.low_band_max_boost_pct;
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.low_band_stage_activated",
                        .detail = detail.str(),
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = true,
                    });
            }

            if (channel.low_band_stage_active) {
                const double scale =
                    SmoothScale(state.debt, threshold, 1.0);
                target_boost_pct =
                    channel.config.low_band_max_boost_pct * scale;
            }
        } else {
            channel.low_band_stage_active = false;
            channel.low_band_eligible_ms = 0u;
        }

        channel.low_band_stage_boost_pct = std::clamp(
            MoveTowardRateLimited(
                channel.low_band_stage_boost_pct,
                target_boost_pct,
                dt_minutes,
                cfg.stage_rise_pct_per_min,
                cfg.stage_fall_pct_per_min),
            0.0,
            configured ? channel.config.low_band_max_boost_pct : 0.0);

        if (channel.low_band_stage_boost_pct > 0.0005) {
            ++channel.low_band_active_sample_count;
            channel.low_band_boost_area_pct_s +=
                channel.low_band_stage_boost_pct * dt_seconds;
            channel.low_band_max_boost_pct = (std::max)(
                channel.low_band_max_boost_pct,
                channel.low_band_stage_boost_pct);
        }

        if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                runtime_snapshot, channel.config.channel)) {
            if (fan->tach_valid) {
                if (channel.low_band_stage_boost_pct > 0.0005) {
                    channel.low_band_boosted_rpm_sum +=
                        static_cast<double>(fan->rpm);
                    ++channel.low_band_boosted_rpm_count;
                } else {
                    channel.low_band_unboosted_rpm_sum +=
                        static_cast<double>(fan->rpm);
                    ++channel.low_band_unboosted_rpm_count;
                }
            }
        }
    }
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

    TryWriteJsonFileAtomic(context.runtime_home / "low_band_evidence.json",
                           payload);
}

void CaptureChannelBaselineIfAvailable(
    const ControlRuntimeContext& context,
    ChannelState& channel,
    const RuntimeSnapshot& runtime_snapshot,
    std::uint64_t tick_count) {
    if (channel.baseline_captured) {
        return;
    }

    const RuntimeFanSnapshot* fan =
        FindRuntimeFanChannel(runtime_snapshot, channel.config.channel);
    if (fan == nullptr) {
        return;
    }

    channel.baseline_duty_raw = fan->duty_raw;
    channel.baseline_mode_raw = fan->mode_raw;
    channel.baseline_captured = true;
    channel.last_issued_pct = fan->duty_percent;
    channel.last_setpoint_pct = fan->duty_percent;
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

void AppendChannelSensorEvent(const ControlRuntimeContext& context,
                              const ChannelState& channel,
                              const ChannelEvaluation& evaluation,
                              std::uint64_t tick_count) {
    if (evaluation.sensor_event == ChannelSensorEvent::FailureDetected) {
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
        return;
    }

    if (evaluation.sensor_event == ChannelSensorEvent::Recovered) {
        AppendRuntimeEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.sensor_recovered",
                .detail = "sensor recovered, resuming normal control",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .observed_temp_c = evaluation.sensor_event_observed_temp_c,
                .success = true,
            });
    }
}

}  // namespace

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
    ClearRuntimeStopRequest(context.runtime_home);
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(context.runtime_home).string();
    RuntimeCsvLogger csv_logger(
        context.runtime_home,
        context.base.log_rotate_hours,
        context.base.log_retain_days,
        context.base.csv_flush_interval_rows);
    std::string log_csv_path;
    std::string log_manifest_path;
    if (csv_logger.Open("control-loop", BuildControlLoopCsvHeader())) {
        log_csv_path = csv_logger.active_archive_path().string();
        log_manifest_path = csv_logger.manifest_path().string();
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
                        last_timing, context.channels, log_csv_path,
                        log_manifest_path, event_log_path);
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
                        last_timing, context.channels, log_csv_path,
                        log_manifest_path, event_log_path);
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
    std::string last_successful_restore_iso;
    std::chrono::steady_clock::time_point previous_tick_start;
    bool have_previous_tick_start = false;
    // Throttle snapshot-file writes: per-tick atomic rename is expensive on
    // Windows and the per-tick CSV row already records the same state. The
    // read-loop process publishes snapshot.json at full poll rate; the
    // control loop only needs to keep it loosely fresh for status consumers.
    std::chrono::steady_clock::time_point last_snapshot_write_time;
    bool have_last_snapshot_write_time = false;
    constexpr std::chrono::milliseconds kSnapshotWriteMinInterval{1000};
    std::chrono::steady_clock::time_point last_low_band_evidence_write_time;
    bool have_last_low_band_evidence_write_time = false;

    // In-memory pending-writes cache. Upserts still persist synchronously
    // (crash-recovery contract); removals are batched and flushed at the
    // end of each tick so we avoid an extra JSON parse + serialize +
    // atomic rename per channel per tick on the happy path.
    PendingWritesStore pending_store(context.runtime_home);
    try {
        pending_store.Load();
    } catch (const std::exception& e) {
        AppendRuntimeEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.sidecar_load_failed",
                .detail = std::string(
                              "failed to load pending-writes sidecar; "
                              "continuing with empty cache: ") +
                          e.what(),
                .success = false,
            });
    }
    const std::uint32_t processor_count = ActiveProcessorCount();
    ProcessResourceSample resource_window_sample = SampleProcessResources();
    bool have_resource_window_sample = resource_window_sample.valid_cpu ||
        resource_window_sample.valid_memory;
    double last_process_cpu_delta_ms =
        std::numeric_limits<double>::quiet_NaN();
    double last_process_cpu_pct = std::numeric_limits<double>::quiet_NaN();

    while (!stop_flag.load() &&
           !RuntimeStopRequested(context.runtime_home)) {
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
            log_manifest_path = csv_logger.manifest_path().string();
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

        std::uint64_t low_band_elapsed_ms = context.loop.poll_tick_ms;
        if (std::isfinite(achieved_interval_ms) &&
            achieved_interval_ms > 0.0) {
            low_band_elapsed_ms =
                static_cast<std::uint64_t>(achieved_interval_ms + 0.5);
        }
        UpdateLowBandState(context, temp_inputs, runtime_snapshot,
                           low_band_elapsed_ms, now_steady, tick_count);

        if (runtime_snapshot_available) {
            const bool snapshot_write_due =
                !have_last_snapshot_write_time ||
                (now_steady - last_snapshot_write_time) >=
                    kSnapshotWriteMinInterval;
            if (snapshot_write_due) {
                WriteRuntimeSnapshotFile(context.runtime_home,
                                         runtime_snapshot);
                last_snapshot_write_time = now_steady;
                have_last_snapshot_write_time = true;
            }
        }

        // Per-channel decisions.
        for (auto& channel : context.channels) {
            const ChannelTimingConfig channel_timing =
                BuildChannelTimingConfig(context.loop, channel, now_steady);

            CaptureChannelBaselineIfAvailable(
                context, channel, runtime_snapshot, tick_count);

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
                    last_successful_restore_iso =
                        FormatLocalIso8601(std::chrono::system_clock::now());
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
                        pending_store.QueueRemove(channel.config.channel);
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
            AppendChannelSensorEvent(context, channel, evaluation, tick_count);
            if (!evaluation.has_setpoint) {
                continue;
            }

            const double observed_temp_c = evaluation.observed_temp_c;
            const double setpoint = evaluation.setpoint_pct;
            const bool first_write = std::isnan(channel.last_issued_pct);
            const std::string write_reason =
                first_write ? std::string("first_write")
                            : (evaluation.authority_reassert
                                   ? std::string("authority_reassert")
                                   : std::string("setpoint_delta"));

            if (!std::isnan(channel.last_issued_pct)) {
                const double delta = std::abs(setpoint - channel.last_issued_pct);
                if (!evaluation.authority_reassert &&
                    delta < evaluation.timing.effective_deadband_pct) {
                    continue;
                }
            }

            if (first_write) {
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
                pending_store.Upsert(entry);
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
                channel.last_write_reason = "write_failed";
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
                        pending_store.QueueRemove(channel.config.channel);
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
            channel.last_write_reason = write_reason;
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.write_applied",
                    .detail = "applied control-loop setpoint source=" +
                              evaluation.response_source +
                              " write_reason=" + write_reason,
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .observed_temp_c = observed_temp_c,
                    .setpoint_pct = setpoint,
                    .success = true,
                });
        }

        // Flush any queued pending-write removals once per tick. Upserts
        // already persisted synchronously inside the loop.
        try {
            pending_store.Flush();
        } catch (const std::exception& e) {
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.sidecar_flush_failed",
                    .detail =
                        std::string("end-of-tick sidecar flush failed: ") +
                        e.what(),
                    .tick_count = tick_count,
                    .success = false,
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

        if (context.loop.low_band.enabled) {
            const auto evidence_interval = std::chrono::milliseconds(
                context.loop.low_band.evidence_write_interval_ms);
            const bool evidence_due =
                !have_last_low_band_evidence_write_time ||
                (tick_finished_steady - last_low_band_evidence_write_time) >=
                    evidence_interval;
            if (evidence_due) {
                WriteLowBandEvidenceFile(context, tick_count);
                last_low_band_evidence_write_time = tick_finished_steady;
                have_last_low_band_evidence_write_time = true;
            }
        }

        // Rate-limit status file writes to reduce disk I/O. CSV remains per tick.
        // Write every kStatusUpdateIntervalTicks ticks (= 10 x poll_tick_ms;
        // 2.5 s at the shipped 250 ms cadence).
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
                            context.channels, log_csv_path, log_manifest_path,
                            event_log_path, last_successful_restore_iso);
        }

        WaitForNextControlTick(context, tick_started_steady, stop_flag);
    }

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    "stop requested", tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, context.channels, log_csv_path,
                    log_manifest_path, event_log_path,
                    last_successful_restore_iso);
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
            last_successful_restore_iso =
                FormatLocalIso8601(std::chrono::system_clock::now());
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
            pending_store.QueueRemove(channel.config.channel);
            pending_store.Flush();
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
                    last_timing, context.channels, log_csv_path,
                    log_manifest_path, event_log_path,
                    last_successful_restore_iso);
    if (context.loop.low_band.enabled) {
        WriteLowBandEvidenceFile(context, tick_count);
    }
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
