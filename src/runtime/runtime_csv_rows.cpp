#include "runtime_csv_rows.h"

#include "csv_util.h"
#include "gpu_evidence_csv.h"
#include "runtime_artifacts.h"
#include "runtime_util.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace svg_mb_control {

namespace {

double FindMaxCpuTemperature(const RuntimeSnapshot& snapshot) {
    double max_temp = std::numeric_limits<double>::quiet_NaN();
    for (const auto& sensor : snapshot.amd_sensors) {
        if (std::isnan(max_temp) || sensor.temperature_c > max_temp) {
            max_temp = sensor.temperature_c;
        }
    }
    return max_temp;
}

// Generic linear lookup used by the per-tick row builder. Member-pointer
// based so the four typed Find* wrappers below collapse to a one-liner
// each and stay strongly typed for the caller.
template <class T, class Key>
const T* FindByKey(const std::vector<T>& items,
                   Key T::*member,
                   Key key) {
    for (const auto& entry : items) {
        if (entry.*member == key) {
            return &entry;
        }
    }
    return nullptr;
}

const RuntimeControlChannelLogState* FindChannelState(
    const std::vector<RuntimeControlChannelLogState>& channels,
    std::uint32_t channel) {
    return FindByKey(channels,
                     &RuntimeControlChannelLogState::channel, channel);
}

const RuntimeSioVoltageLogState* FindSioVoltageState(
    const std::vector<RuntimeSioVoltageLogState>& voltages,
    std::uint32_t index) {
    return FindByKey(voltages,
                     &RuntimeSioVoltageLogState::index, index);
}

const RuntimeFanTachEvidenceLogState* FindFanTachEvidenceState(
    const std::vector<RuntimeFanTachEvidenceLogState>& fans,
    std::uint32_t channel) {
    return FindByKey(fans,
                     &RuntimeFanTachEvidenceLogState::channel, channel);
}

const RuntimeSioTemperatureLogState* FindSioTemperatureState(
    const std::vector<RuntimeSioTemperatureLogState>& temperatures,
    std::uint32_t index) {
    return FindByKey(temperatures,
                     &RuntimeSioTemperatureLogState::index, index);
}

// Fills `out` with "label=temp | label=temp | ..." using fixed 3-decimal
// temperatures. Writes into the caller-provided buffer (cleared first) so a
// reused buffer's capacity is retained across per-tick calls instead of
// allocating a fresh std::ostringstream + string every row. The "%.3f"
// formatting matches the previous std::fixed/precision(3) ostream output for
// finite values.
void BuildAmdSensorSummary(const RuntimeSnapshot& snapshot, std::string& out) {
    out.clear();
    bool first = true;
    char number[32];
    for (const auto& sensor : snapshot.amd_sensors) {
        if (!first) {
            out += " | ";
        }
        first = false;
        out += sensor.label;
        out += '=';
        const int written = std::snprintf(number, sizeof(number), "%.3f",
                                           sensor.temperature_c);
        if (written > 0) {
            out.append(number,
                       static_cast<std::size_t>(
                           written < static_cast<int>(sizeof(number))
                               ? written
                               : static_cast<int>(sizeof(number)) - 1));
        }
    }
}

std::string BuildCommonCsvHeader() {
    std::ostringstream header;
    header
        << "wall_clock,mode,snapshot_time,snapshot_age_ms,"
        << "amd_sensor_count,amd_sensor_summary,cpu_tctl_c,cpu_max_c,"
        << "gpu_available,gpu_name,gpu_last_warning,"
        << "gpu_core_c,gpu_memjn_c,gpu_hotspot_c,"
        << "fan_count,policy_writes_enabled_present,policy_writes_enabled";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",fan" << channel << "_present"
               << ",fan" << channel << "_label"
               << ",fan" << channel << "_rpm"
               << ",fan" << channel << "_tach_raw"
               << ",fan" << channel << "_tach_valid"
               << ",fan" << channel << "_duty_raw"
               << ",fan" << channel << "_duty_pct"
               << ",fan" << channel << "_mode_raw"
               << ",fan" << channel << "_manual_override"
               << ",fan" << channel << "_write_allowed"
               << ",fan" << channel << "_policy_blocked"
               << ",fan" << channel << "_effective_write_allowed";
    }
    return header.str();
}

// Writes the shared leading CSV columns directly into the caller's stream.
// Previously this built its own std::ostringstream and returned a heap
// std::string that the row builder then copied in; writing in place removes
// one ostringstream + one string allocation/copy per row.
void BuildCommonCsvPrefix(std::ostringstream& csv,
                          const RuntimeSnapshot& snapshot,
                          std::string_view mode) {
    AppendCsvString(csv, FormatLocalIso8601(
                             std::chrono::system_clock::now()));
    AppendCsvFieldString(csv, mode);
    AppendCsvFieldString(csv, snapshot.snapshot_time_iso);
    csv << ',';
    if (const auto parsed = ParseLocalIso8601(snapshot.snapshot_time_iso)) {
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - *parsed).count();
        csv << age_ms;
    }
    AppendCsvField(csv, snapshot.amd_sensors.size());
    thread_local std::string amd_summary;
    BuildAmdSensorSummary(snapshot, amd_summary);
    AppendCsvFieldString(csv, amd_summary);
    AppendCsvFieldDouble(csv,
                         FindRuntimeAmdSensorTemperature(snapshot, "Tctl/Tdie"));
    AppendCsvFieldDouble(csv, FindMaxCpuTemperature(snapshot));
    AppendCsvFieldBool(csv, snapshot.gpu.available);
    AppendCsvFieldString(csv, snapshot.gpu.gpu_name);
    AppendCsvFieldString(csv, snapshot.gpu.last_warning);
    AppendCsvFieldDouble(csv, snapshot.gpu.core_c);
    AppendCsvFieldDouble(csv, snapshot.gpu.memjn_c);
    AppendCsvFieldDouble(csv, snapshot.gpu.hotspot_c);
    AppendCsvField(csv, snapshot.fans.size());
    AppendCsvFieldBool(csv, snapshot.policy_writes_enabled_present);
    AppendCsvFieldBoolIf(csv, snapshot.policy_writes_enabled_present,
                         snapshot.policy_writes_enabled);

    static const RuntimeFanSnapshot kEmptyFan{};
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeFanSnapshot* found =
            FindRuntimeFanChannel(snapshot, channel);
        const bool present = found != nullptr;
        const RuntimeFanSnapshot& fan = present ? *found : kEmptyFan;
        AppendCsvFieldBool(csv, present);
        AppendCsvFieldStringIf(csv, present, fan.label);
        AppendCsvFieldIf(csv, present, fan.rpm);
        AppendCsvFieldIf(csv, present, fan.tach_raw);
        AppendCsvFieldBoolIf(csv, present, fan.tach_valid);
        AppendCsvFieldIf(csv, present, static_cast<unsigned int>(fan.duty_raw));
        AppendCsvFieldDoubleIf(csv, present, fan.duty_percent, 2);
        AppendCsvFieldIf(csv, present, static_cast<unsigned int>(fan.mode_raw));
        AppendCsvFieldBoolIf(csv, present, fan.manual_override);
        AppendCsvFieldBoolIf(csv, present, fan.write_allowed);
        AppendCsvFieldBoolIf(csv, present, fan.policy_blocked);
        AppendCsvFieldBoolIf(csv, present, fan.effective_write_allowed);
    }
}

}  // namespace

std::string BuildReadLoopCsvHeader() {
    std::ostringstream header;
    header << BuildCommonCsvHeader()
           << ",telemetry_available"
           << ",runtime_home_published"
           << ",snapshot_mirror_configured"
           << ",snapshot_mirror_published"
           << ",successful_polls"
           << ",skipped_polls"
           << ",stale"
           << ",status_detail";
    return header.str();
}

std::string BuildReadLoopCsvRow(const RuntimeSnapshot& snapshot,
                                const RuntimeReadLoopLogState& state) {
    std::ostringstream csv;
    BuildCommonCsvPrefix(csv, snapshot, "read-loop");
    AppendCsvFieldBool(csv, state.telemetry_available);
    AppendCsvFieldBool(csv, state.runtime_home_published);
    AppendCsvFieldBool(csv, state.snapshot_mirror_configured);
    AppendCsvFieldBool(csv, state.snapshot_mirror_published);
    AppendCsvField(csv, state.successful_polls);
    AppendCsvField(csv, state.skipped_polls);
    AppendCsvFieldBool(csv, state.stale);
    AppendCsvFieldString(csv, state.status_detail);
    return csv.str();
}

std::string BuildEvidenceLogCsvHeader() {
    std::ostringstream header;
    header << BuildCommonCsvHeader()
           << ",telemetry_available"
           << ",successful_polls"
           << ",skipped_polls"
           << ",stale"
           << ",status_detail"
           << ",evidence_poll_interval_ms"
           << ",evidence_sample_duration_ms"
           << ",runtime_snapshot_read_ms"
           << ",amd_read_ms"
           << ",gpu_thermal_read_ms"
           << ",fan_scan_read_ms"
           << ",fan_tach_read_ms"
           << ",sio_voltage_read_ms"
           << ",sio_temperature_read_ms"
           << ",gpu_evidence_read_ms"
           << ",runtime_snapshot_changed"
           << ",amd_changed"
           << ",gpu_thermal_changed"
           << ",fan_state_changed"
           << ",fan_tach_changed"
           << ",sio_voltage_changed"
           << ",sio_temperature_changed"
           << ",gpu_evidence_changed"
           << ",sio_evidence_available"
           << ",sio_evidence_detail"
           << ",sio_voltage_count"
           << ",sio_temperature_count";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",fan" << channel << "_tach_hi_raw"
               << ",fan" << channel << "_tach_lo_raw";
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioVoltageCount);
         ++index) {
        header << ",sio_voltage" << index << "_present"
               << ",sio_voltage" << index << "_label"
               << ",sio_voltage" << index << "_raw"
               << ",sio_voltage" << index << "_v";
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioTemperatureCount);
         ++index) {
        header << ",sio_temp" << index << "_present"
               << ",sio_temp" << index << "_label"
               << ",sio_temp" << index << "_raw"
               << ",sio_temp" << index << "_half_raw"
               << ",sio_temp" << index << "_valid"
               << ",sio_temp" << index << "_c";
    }
    header << BuildGpuEvidenceCsvHeader();
    return header.str();
}

std::string BuildEvidenceLogCsvRow(const RuntimeSnapshot& snapshot,
                                   const RuntimeEvidenceLogState& state) {
    std::ostringstream csv;
    BuildCommonCsvPrefix(csv, snapshot, "evidence-log");
    AppendCsvFieldBool(csv, state.telemetry_available);
    AppendCsvField(csv, state.successful_polls);
    AppendCsvField(csv, state.skipped_polls);
    AppendCsvFieldBool(csv, state.stale);
    AppendCsvFieldString(csv, state.status_detail);
    AppendCsvFieldDouble(csv, state.evidence_poll_interval_ms);
    AppendCsvFieldDouble(csv, state.evidence_sample_duration_ms);
    AppendCsvFieldDouble(csv, state.runtime_snapshot_read_ms);
    AppendCsvFieldDouble(csv, state.amd_read_ms);
    AppendCsvFieldDouble(csv, state.gpu_thermal_read_ms);
    AppendCsvFieldDouble(csv, state.fan_scan_read_ms);
    AppendCsvFieldDouble(csv, state.fan_tach_read_ms);
    AppendCsvFieldDouble(csv, state.sio_voltage_read_ms);
    AppendCsvFieldDouble(csv, state.sio_temperature_read_ms);
    AppendCsvFieldDouble(csv, state.gpu_evidence_read_ms);
    AppendCsvFieldBool(csv, state.runtime_snapshot_changed);
    AppendCsvFieldBool(csv, state.amd_changed);
    AppendCsvFieldBool(csv, state.gpu_thermal_changed);
    AppendCsvFieldBool(csv, state.fan_state_changed);
    AppendCsvFieldBool(csv, state.fan_tach_changed);
    AppendCsvFieldBool(csv, state.sio_voltage_changed);
    AppendCsvFieldBool(csv, state.sio_temperature_changed);
    AppendCsvFieldBool(csv, state.gpu_evidence_changed);
    AppendCsvFieldBool(csv, state.sio_evidence_available);
    AppendCsvFieldString(csv, state.sio_evidence_detail);
    AppendCsvField(csv, state.sio_voltages.size());
    AppendCsvField(csv, state.sio_temperatures.size());

    static const RuntimeFanTachEvidenceLogState kEmptyTach{};
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeFanTachEvidenceLogState* found =
            FindFanTachEvidenceState(state.fan_tach_evidence, channel);
        const bool present = found != nullptr;
        const RuntimeFanTachEvidenceLogState& fan =
            present ? *found : kEmptyTach;
        AppendCsvFieldIf(csv, present,
                         static_cast<unsigned int>(fan.tach_hi_raw));
        AppendCsvFieldIf(csv, present,
                         static_cast<unsigned int>(fan.tach_lo_raw));
    }

    static const RuntimeSioVoltageLogState kEmptyVoltage{};
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioVoltageCount);
         ++index) {
        const RuntimeSioVoltageLogState* found =
            FindSioVoltageState(state.sio_voltages, index);
        const bool present = found != nullptr;
        const RuntimeSioVoltageLogState& voltage =
            present ? *found : kEmptyVoltage;
        AppendCsvFieldBool(csv, present);
        AppendCsvFieldStringIf(csv, present, voltage.label);
        AppendCsvFieldIf(csv, present, static_cast<unsigned int>(voltage.raw));
        AppendCsvFieldDoubleIf(csv, present, voltage.voltage_v, 4);
    }

    static const RuntimeSioTemperatureLogState kEmptyTemperature{};
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioTemperatureCount);
         ++index) {
        const RuntimeSioTemperatureLogState* found =
            FindSioTemperatureState(state.sio_temperatures, index);
        const bool present = found != nullptr;
        const RuntimeSioTemperatureLogState& temperature =
            present ? *found : kEmptyTemperature;
        AppendCsvFieldBool(csv, present);
        AppendCsvFieldStringIf(csv, present, temperature.label);
        AppendCsvFieldIf(csv, present,
                         static_cast<unsigned int>(temperature.raw));
        AppendCsvFieldIf(csv, present,
                         static_cast<unsigned int>(temperature.half_raw));
        AppendCsvFieldBoolIf(csv, present, temperature.valid);
        AppendCsvFieldDoubleIf(csv, present, temperature.temperature_c);
    }
    AppendGpuEvidenceCsvRow(csv, state.gpu_evidence);
    return csv.str();
}

std::string BuildControlLoopCsvHeader() {
    std::ostringstream header;
    header << BuildCommonCsvHeader()
           << ",loop_tick_count"
           << ",loop_started_wall_clock"
           << ",loop_finished_wall_clock"
           << ",loop_work_duration_ms"
           << ",loop_intended_interval_ms"
           << ",loop_achieved_interval_ms"
           << ",loop_slip_ms"
           << ",loop_overrun"
           << ",process_cpu_delta_ms"
           << ",process_cpu_pct"
           << ",process_working_set_bytes"
           << ",process_private_bytes"
           << ",cadence_transient";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",channel" << channel << "_observed_temp_c"
               << ",channel" << channel << "_setpoint_pct"
               << ",channel" << channel << "_thermal_pressure_boost_pct"
               << ",channel" << channel << "_midband_pressure_boost_pct"
               << ",channel" << channel << "_gpu_airflow_boost_pct"
               << ",channel" << channel << "_cpu_low_soak_boost_pct"
               << ",channel" << channel << "_low_band_stage_boost_pct"
               << ",channel" << channel << "_low_band_effective_boost_pct"
               << ",channel" << channel << "_low_band_debt"
               << ",channel" << channel << "_low_band_signal"
               << ",channel" << channel << "_low_band_stage_active"
               << ",channel" << channel << "_response_source"
               << ",channel" << channel << "_write_reason"
               << ",channel" << channel << "_total_writes"
               << ",channel" << channel << "_write_active"
               << ",channel" << channel << "_baseline_captured"
               << ",channel" << channel << "_feedforward_pct"
               << ",channel" << channel << "_correction_pct";
    }
    return header.str();
}

std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    std::uint64_t tick_count,
    const RuntimeControlLoopTimingState& timing,
    const std::vector<RuntimeControlChannelLogState>& channels) {
    std::ostringstream csv;
    BuildCommonCsvPrefix(csv, snapshot, "control-loop");
    AppendCsvField(csv, tick_count);
    AppendCsvFieldString(csv, timing.loop_started_wall_clock);
    AppendCsvFieldString(csv, timing.loop_finished_wall_clock);
    AppendCsvFieldDouble(csv, timing.loop_work_duration_ms);
    AppendCsvField(csv, timing.loop_intended_interval_ms);
    AppendCsvFieldDouble(csv, timing.loop_achieved_interval_ms);
    AppendCsvFieldDouble(csv, timing.loop_slip_ms);
    AppendCsvFieldBool(csv, timing.loop_overrun);
    AppendCsvFieldDouble(csv, timing.process_cpu_delta_ms);
    AppendCsvFieldDouble(csv, timing.process_cpu_pct);
    AppendCsvField(csv, timing.process_working_set_bytes);
    AppendCsvField(csv, timing.process_private_bytes);
    AppendCsvFieldDouble(csv, timing.cadence_transient);

    static const RuntimeControlChannelLogState kEmptyChannel{};
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeControlChannelLogState* found =
            FindChannelState(channels, channel);
        const bool present = found != nullptr;
        const RuntimeControlChannelLogState& state =
            present ? *found : kEmptyChannel;
        AppendCsvFieldDoubleIf(csv, present, state.observed_temp_c);
        AppendCsvFieldDoubleIf(csv, present, state.setpoint_pct);
        AppendCsvFieldDoubleIf(csv, present, state.thermal_pressure_boost_pct);
        AppendCsvFieldDoubleIf(csv, present, state.midband_pressure_boost_pct);
        AppendCsvFieldDoubleIf(csv, present, state.gpu_airflow_boost_pct);
        AppendCsvFieldDoubleIf(csv, present, state.cpu_low_soak_boost_pct);
        AppendCsvFieldDoubleIf(csv, present, state.low_band_stage_boost_pct);
        AppendCsvFieldDoubleIf(csv, present,
                               state.low_band_effective_boost_pct);
        AppendCsvFieldDoubleIf(csv, present, state.low_band_debt);
        AppendCsvFieldDoubleIf(csv, present, state.low_band_signal);
        AppendCsvFieldBoolIf(csv, present, state.low_band_stage_active);
        AppendCsvFieldStringIf(csv, present, state.response_source);
        AppendCsvFieldStringIf(csv, present, state.write_reason);
        AppendCsvFieldIf(csv, present, state.total_writes);
        AppendCsvFieldBoolIf(csv, present, state.write_active);
        AppendCsvFieldBoolIf(csv, present, state.baseline_captured);
        AppendCsvFieldDoubleIf(csv, present, state.feedforward_pct);
        const bool correction_present = present &&
                                        !std::isnan(state.setpoint_pct) &&
                                        !std::isnan(state.feedforward_pct);
        AppendCsvFieldDoubleIf(csv, correction_present,
                               state.setpoint_pct - state.feedforward_pct);
    }
    return csv.str();
}

}  // namespace svg_mb_control
