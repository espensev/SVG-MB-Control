#include "channel_write.h"

#include "control_scheduler.h"
#include "runtime_event_log.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace svg_mb_control {

namespace {

std::string ResolveWriteReason(bool first_write, bool authority_reassert) {
    if (first_write) {
        return "first_write";
    }
    return authority_reassert ? "authority_reassert" : "setpoint_delta";
}

bool RuntimeFanAllowsWrite(const RuntimeSnapshot& runtime_snapshot,
                           std::uint32_t channel_id) {
    const RuntimeFanSnapshot* fan =
        FindRuntimeFanChannel(runtime_snapshot, channel_id);
    return fan == nullptr || fan->effective_write_allowed;
}

// Bookkeeping + event emissions for a failed ApplyDuty. Bumps the consecutive
// failure counter, opens the channel circuit breaker once the threshold is
// crossed, removes the pending-write sidecar when the failure was a policy
// refusal, and emits the write_failed event. Caller returns immediately
// after; no further setpoint-related state is touched.
void HandleChannelWriteFailure(ControlRuntimeContext& context,
                               ChannelState& channel,
                               PendingWritesStore& pending_store,
                               const FanWriteResult& write_result,
                               double observed_temp_c,
                               double setpoint,
                               std::uint64_t tick_count) {
    channel.last_write_reason = "write_failed";
    ++channel.consecutive_write_failures;
    if (channel.consecutive_write_failures >=
            ChannelState::kMaxConsecutiveFailures &&
        !channel.circuit_breaker_open) {
        channel.circuit_breaker_open = true;
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.circuit_breaker_opened",
                .detail =
                    "circuit breaker opened after repeated write failures",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .observed_temp_c = observed_temp_c,
                .setpoint_pct = setpoint,
                .success = false,
            });
    }

    if (write_result.error == FanWriteError::kPolicyRefused) {
        try {
            pending_store.QueueRemove(channel.config.channel);
        } catch (const std::exception& e) {
            AppendControlLoopEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .event_type = "control_loop.sidecar_remove_warning",
                    .detail = std::string(
                                  "best-effort sidecar removal after "
                                  "policy refusal failed: ") +
                              e.what(),
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = false,
                });
        }
    }
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.write_failed",
            .detail = write_result.detail,
            .channel = channel.config.channel,
            .tick_count = tick_count,
            .observed_temp_c = observed_temp_c,
            .setpoint_pct = setpoint,
            .success = false,
        });
}

// Bookkeeping after a successful ApplyDuty: clear the consecutive-failure
// counter and, if the breaker was open, close it and emit the recovery event.
void NoteSuccessfulChannelWrite(ControlRuntimeContext& context,
                                ChannelState& channel,
                                double observed_temp_c,
                                double setpoint,
                                std::uint64_t tick_count) {
    if (channel.consecutive_write_failures == 0u &&
        !channel.circuit_breaker_open) {
        return;
    }
    if (channel.circuit_breaker_open) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.circuit_breaker_closed",
                .detail =
                    "circuit breaker closed after successful write",
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

}  // namespace

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
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
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
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.sensor_failure_detected",
                .detail = "sensor failure detected, entering safe mode (100% duty)",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .success = false,
            });
        return;
    }

    if (evaluation.sensor_event == ChannelSensorEvent::Recovered) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.sensor_recovered",
                .detail = "sensor recovered, resuming normal control",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .observed_temp_c = evaluation.sensor_event_observed_temp_c,
                .success = true,
            });
    }
}

bool HandleExpiredHoldRestore(
    ControlRuntimeContext& context,
    ChannelState& channel,
    const ChannelTimingConfig& channel_timing,
    FanWriter& fan_writer,
    PendingWritesStore& pending_store,
    std::chrono::steady_clock::time_point now_steady,
    std::uint64_t tick_count,
    std::string& last_successful_restore_iso,
    bool& fatal_restore_timeout,
    std::uint32_t& fatal_restore_channel) {
    if (!channel.write_active ||
        channel_timing.effective_hold_ms == 0u ||
        now_steady < channel.hold_deadline ||
        !channel.baseline_captured) {
        return true;
    }

    const FanWriteResult restore_result =
        fan_writer.RestoreSavedState(channel.config.channel,
                                     channel.baseline_duty_raw,
                                     channel.baseline_mode_raw,
                                     context.base.restore_timeout_ms);
    if (restore_result) {
        last_successful_restore_iso =
            FormatLocalIso8601(std::chrono::system_clock::now());
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.restore_applied",
                .detail = "restored channel to captured baseline",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .success = true,
            });
        channel.write_active = false;
        channel.last_issued_pct = std::numeric_limits<double>::quiet_NaN();
        try {
            pending_store.QueueRemove(channel.config.channel);
        } catch (const std::exception& e) {
            AppendControlLoopEvent(
                context.runtime_home,
                RuntimeLogEvent{
                            .event_type = "control_loop.sidecar_remove_warning",
                    .detail = std::string(
                                  "best-effort sidecar removal after "
                                  "restore failed: ") +
                              e.what(),
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = false,
                });
        }
        return true;
    }

    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.restore_failed",
            .detail = restore_result.detail,
            .channel = channel.config.channel,
            .tick_count = tick_count,
            .success = false,
        });
    if (restore_result.error == FanWriteError::kTimedOut) {
        fatal_restore_timeout = true;
        fatal_restore_channel = channel.config.channel;
        return false;
    }
    return true;
}

void TryApplyChannelSetpoint(
    ControlRuntimeContext& context,
    ChannelState& channel,
    const RuntimeSnapshot& runtime_snapshot,
    const ChannelEvaluation& evaluation,
    FanWriter& fan_writer,
    PendingWritesStore& pending_store,
    std::string_view eval_iso,
    std::chrono::steady_clock::time_point now_steady,
    std::uint64_t tick_count) {
    if (!evaluation.has_setpoint) {
        return;
    }

    const double observed_temp_c = evaluation.observed_temp_c;
    const double setpoint = evaluation.setpoint_pct;
    const bool first_write = std::isnan(channel.last_issued_pct);
    const std::string write_reason =
        ResolveWriteReason(first_write, evaluation.authority_reassert);

    if (!std::isnan(channel.last_issued_pct)) {
        const double delta = std::abs(setpoint - channel.last_issued_pct);
        if (!evaluation.authority_reassert &&
            delta < evaluation.timing.effective_deadband_pct) {
            return;
        }
    }

    if (!first_write &&
        evaluation.timing.elapsed_since_last_write_ms <
            WriteCooldownForAuthorityReassert(
                evaluation.timing, evaluation.authority_reassert)) {
        return;
    }

    if (!channel.baseline_captured) {
        return;
    }
    if (!RuntimeFanAllowsWrite(runtime_snapshot, channel.config.channel)) {
        return;
    }
    if (channel.circuit_breaker_open) {
        return;
    }

    PendingWriteEntry entry;
    entry.channel = channel.config.channel;
    entry.baseline_duty_raw = channel.baseline_duty_raw;
    entry.baseline_mode_raw = channel.baseline_mode_raw;
    entry.target_pct = setpoint;
    entry.requested_hold_ms = evaluation.timing.effective_hold_ms;
    entry.started_iso = std::string(eval_iso);
    entry.child_pid = 0u;
    try {
        pending_store.Upsert(entry);
    } catch (const std::exception& e) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.sidecar_upsert_failed",
                .detail = std::string(
                              "failed to write sidecar entry before "
                              "fan write: ") +
                          e.what(),
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .success = false,
            });
        return;
    }

    const FanWriteResult write_result =
        fan_writer.ApplyDuty(channel.config.channel, setpoint);
    if (!write_result) {
        HandleChannelWriteFailure(context, channel, pending_store,
                                  write_result, observed_temp_c, setpoint,
                                  tick_count);
        return;
    }

    NoteSuccessfulChannelWrite(context, channel, observed_temp_c, setpoint,
                               tick_count);
    if (evaluation.authority_reassert) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
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
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
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

}  // namespace svg_mb_control
