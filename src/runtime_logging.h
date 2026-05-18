#pragma once

#include "gpu_reader.h"
#include "runtime_snapshot.h"

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
    double thermal_pressure_boost_pct = 0.0;
    double cpu_low_soak_boost_pct = 0.0;
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
    std::uint64_t tick_count,
    const RuntimeControlLoopTimingState& timing,
    const std::vector<RuntimeControlChannelLogState>& channels);

}  // namespace svg_mb_control
