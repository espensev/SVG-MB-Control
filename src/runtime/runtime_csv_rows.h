#pragma once

#include "boost_stage.h"
#include "gpu_reader.h"
#include "runtime_snapshot.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace svg_mb_control {

constexpr std::size_t kRuntimeLogFanChannelCount = 7u;
constexpr std::size_t kRuntimeLogSioVoltageCount = 16u;
constexpr std::size_t kRuntimeLogSioTemperatureCount = 23u;

struct RuntimeControlChannelLogState {
    std::uint32_t channel = 0u;
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double setpoint_pct = std::numeric_limits<double>::quiet_NaN();
    double feedforward_pct = std::numeric_limits<double>::quiet_NaN();
    // Per-stage boost contribution snapshot for this channel, indexed by
    // BoostStage. The CSV column order and JSON status key order both
    // come from kBoostStageSpecs, so do not reorder this without checking
    // the existing field-name contract.
    std::array<double, kBoostStageCount> stage_boost_pct{};
    double low_band_stage_boost_pct = 0.0;
    double low_band_effective_boost_pct = 0.0;
    double low_band_debt = 0.0;
    double low_band_signal = 0.0;
    bool low_band_stage_active = false;
    std::string primary_temp_source;
    std::string response_source;
    std::string write_reason;
    std::uint64_t total_writes = 0u;
    bool write_active = false;
    bool baseline_captured = false;
};

struct RuntimeControlLoopTimingState {
    std::string loop_started_wall_clock;
    std::string loop_finished_wall_clock;
    double loop_work_duration_ms = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t loop_intended_interval_ms = 0u;
    double loop_achieved_interval_ms = std::numeric_limits<double>::quiet_NaN();
    double loop_slip_ms = std::numeric_limits<double>::quiet_NaN();
    bool loop_overrun = false;
    double process_cpu_delta_ms = std::numeric_limits<double>::quiet_NaN();
    double process_cpu_pct = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t process_working_set_bytes = 0u;
    std::uint64_t process_private_bytes = 0u;
    // Whole-system CPU activity for this resource window (FEAT-0002). Counters
    // come from GetSystemTimes and are summed across all logical processors, so
    // a *_delta_ms can exceed the wall-clock window (~processor_count x window).
    // system_cpu_busy_pct is already core-normalized to [0,100]; it is not the
    // controller-process cost (that stays process_cpu_pct). All blank/NaN when
    // unavailable. See docs/cpu-settings-evidence-logger-decision-2026-06-04.md.
    double system_cpu_idle_delta_ms = std::numeric_limits<double>::quiet_NaN();
    double system_cpu_kernel_delta_ms = std::numeric_limits<double>::quiet_NaN();
    double system_cpu_user_delta_ms = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t system_cpu_processor_count = 0u;
    double system_cpu_busy_pct = std::numeric_limits<double>::quiet_NaN();
    // Upward-only adaptive cadence transient (Phase 2): the unitless [0,1]
    // slew score that produced loop_intended_interval_ms this tick. 0 when
    // adaptation is off or there is no transient.
    double cadence_transient = 0.0;
};

struct RuntimeReadLoopLogState {
    bool telemetry_available = false;
    bool runtime_home_published = false;
    bool snapshot_mirror_configured = false;
    bool snapshot_mirror_published = false;
    std::uint64_t successful_polls = 0u;
    std::uint64_t skipped_polls = 0u;
    bool stale = true;
    std::string status_detail;
};

struct RuntimeSioVoltageLogState {
    std::uint32_t index = 0u;
    double voltage_v = std::numeric_limits<double>::quiet_NaN();
    std::uint8_t raw = 0u;
    std::string label;
};

struct RuntimeFanTachEvidenceLogState {
    std::uint32_t channel = 0u;
    std::uint8_t tach_hi_raw = 0u;
    std::uint8_t tach_lo_raw = 0u;
};

struct RuntimeSioTemperatureLogState {
    std::uint32_t index = 0u;
    double temperature_c = std::numeric_limits<double>::quiet_NaN();
    std::uint8_t raw = 0u;
    std::uint8_t half_raw = 0u;
    bool valid = false;
    std::string label;
};

struct RuntimeEvidenceLogState {
    bool telemetry_available = false;
    std::uint64_t successful_polls = 0u;
    std::uint64_t skipped_polls = 0u;
    bool stale = true;
    std::string status_detail;
    double evidence_poll_interval_ms = std::numeric_limits<double>::quiet_NaN();
    double evidence_sample_duration_ms = std::numeric_limits<double>::quiet_NaN();
    double runtime_snapshot_read_ms = std::numeric_limits<double>::quiet_NaN();
    double amd_read_ms = std::numeric_limits<double>::quiet_NaN();
    double gpu_thermal_read_ms = std::numeric_limits<double>::quiet_NaN();
    double fan_scan_read_ms = std::numeric_limits<double>::quiet_NaN();
    double fan_tach_read_ms = std::numeric_limits<double>::quiet_NaN();
    double sio_voltage_read_ms = std::numeric_limits<double>::quiet_NaN();
    double sio_temperature_read_ms = std::numeric_limits<double>::quiet_NaN();
    double gpu_evidence_read_ms = std::numeric_limits<double>::quiet_NaN();
    bool runtime_snapshot_changed = false;
    bool amd_changed = false;
    bool gpu_thermal_changed = false;
    bool fan_state_changed = false;
    bool fan_tach_changed = false;
    bool sio_voltage_changed = false;
    bool sio_temperature_changed = false;
    bool gpu_evidence_changed = false;
    bool sio_evidence_available = false;
    std::string sio_evidence_detail;
    std::vector<RuntimeFanTachEvidenceLogState> fan_tach_evidence;
    std::vector<RuntimeSioVoltageLogState> sio_voltages;
    std::vector<RuntimeSioTemperatureLogState> sio_temperatures;
    GpuEvidenceSample gpu_evidence;
};

std::string BuildReadLoopCsvHeader();
std::string BuildReadLoopCsvRow(const RuntimeSnapshot& snapshot,
                                const RuntimeReadLoopLogState& state);

std::string BuildEvidenceLogCsvHeader();
std::string BuildEvidenceLogCsvRow(const RuntimeSnapshot& snapshot,
                                   const RuntimeEvidenceLogState& state);

std::string BuildControlLoopCsvHeader();
std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    const RuntimeSnapshotIndex& snapshot_index,
    std::uint64_t tick_count,
    const RuntimeControlLoopTimingState& timing,
    const std::vector<RuntimeControlChannelLogState>& channels,
    // FEAT-0023 (REQ-MPROFILE-09): additive, observational active-profile
    // identity. Recorded for offline attribution only; never read by control.
    const std::string& active_profile_name = {},
    const std::string& active_profile_source = {});

}  // namespace svg_mb_control
