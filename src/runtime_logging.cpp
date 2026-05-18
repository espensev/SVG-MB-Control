#include "runtime_logging.h"

#include "csv_util.h"
#include "gpu_evidence_csv.h"
#include "runtime_artifacts.h"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>

namespace svg_mb_control {

namespace {

std::optional<std::chrono::system_clock::time_point> ParseLocalIso8601(
    std::string_view iso_text) {
    if (iso_text.empty()) {
        return std::nullopt;
    }

    std::tm tm_value{};
    std::istringstream stream{std::string(iso_text)};
    stream >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm_value.tm_isdst = -1;
    const std::time_t tt = std::mktime(&tm_value);
    if (tt == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(tt);
}

double FindMaxCpuTemperature(const RuntimeSnapshot& snapshot) {
    double max_temp = std::numeric_limits<double>::quiet_NaN();
    for (const auto& sensor : snapshot.amd_sensors) {
        if (std::isnan(max_temp) || sensor.temperature_c > max_temp) {
            max_temp = sensor.temperature_c;
        }
    }
    return max_temp;
}

const RuntimeControlChannelLogState* FindChannelState(
    const std::vector<RuntimeControlChannelLogState>& channels,
    std::uint32_t channel) {
    for (const auto& entry : channels) {
        if (entry.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

const RuntimeSioVoltageLogState* FindSioVoltageState(
    const std::vector<RuntimeSioVoltageLogState>& voltages,
    std::uint32_t index) {
    for (const auto& entry : voltages) {
        if (entry.index == index) {
            return &entry;
        }
    }
    return nullptr;
}

const RuntimeFanTachEvidenceLogState* FindFanTachEvidenceState(
    const std::vector<RuntimeFanTachEvidenceLogState>& fans,
    std::uint32_t channel) {
    for (const auto& entry : fans) {
        if (entry.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

const RuntimeSioTemperatureLogState* FindSioTemperatureState(
    const std::vector<RuntimeSioTemperatureLogState>& temperatures,
    std::uint32_t index) {
    for (const auto& entry : temperatures) {
        if (entry.index == index) {
            return &entry;
        }
    }
    return nullptr;
}

std::string BuildAmdSensorSummary(const RuntimeSnapshot& snapshot) {
    std::ostringstream summary;
    bool first = true;
    for (const auto& sensor : snapshot.amd_sensors) {
        if (!first) {
            summary << " | ";
        }
        first = false;
        summary << sensor.label << '=';
        const std::streamsize old_precision = summary.precision();
        const auto old_flags = summary.flags();
        summary.setf(std::ios::fixed, std::ios::floatfield);
        summary.precision(3);
        summary << sensor.temperature_c;
        summary.flags(old_flags);
        summary.precision(old_precision);
    }
    return summary.str();
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

std::string BuildCommonCsvPrefix(const RuntimeSnapshot& snapshot,
                                 std::string_view mode) {
    std::ostringstream csv;
    AppendCsvString(csv, FormatRuntimeLocalIso8601(
                             std::chrono::system_clock::now()));
    csv << ',';
    AppendCsvString(csv, mode);
    csv << ',';
    AppendCsvString(csv, snapshot.snapshot_time_iso);
    csv << ',';
    if (const auto parsed = ParseLocalIso8601(snapshot.snapshot_time_iso)) {
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - *parsed).count();
        csv << age_ms;
    }
    csv << ',' << snapshot.amd_sensors.size() << ',';
    AppendCsvString(csv, BuildAmdSensorSummary(snapshot));
    csv << ',';
    AppendCsvDouble(csv, FindRuntimeAmdSensorTemperature(snapshot, "Tctl/Tdie"));
    csv << ',';
    AppendCsvDouble(csv, FindMaxCpuTemperature(snapshot));
    csv << ',';
    AppendCsvBool(csv, snapshot.gpu.available);
    csv << ',';
    AppendCsvString(csv, snapshot.gpu.gpu_name);
    csv << ',';
    AppendCsvString(csv, snapshot.gpu.last_warning);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.core_c);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.memjn_c);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.hotspot_c);
    csv << ',' << snapshot.fans.size() << ',';
    AppendCsvBool(csv, snapshot.policy_writes_enabled_present);
    csv << ',';
    if (snapshot.policy_writes_enabled_present) {
        AppendCsvBool(csv, snapshot.policy_writes_enabled);
    }

    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(snapshot, channel);
        csv << ',';
        AppendCsvBool(csv, fan != nullptr);
        csv << ',';
        if (fan != nullptr) {
            AppendCsvString(csv, fan->label);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << fan->rpm;
        }
        csv << ',';
        if (fan != nullptr) {
            csv << fan->tach_raw;
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->tach_valid);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->duty_raw);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvDouble(csv, fan->duty_percent, 2);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->mode_raw);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->manual_override);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->write_allowed);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->policy_blocked);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->effective_write_allowed);
        }
    }

    return csv.str();
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
    csv << BuildCommonCsvPrefix(snapshot, "read-loop") << ',';
    AppendCsvBool(csv, state.telemetry_available);
    csv << ',';
    AppendCsvBool(csv, state.runtime_home_published);
    csv << ',';
    AppendCsvBool(csv, state.snapshot_mirror_configured);
    csv << ',';
    AppendCsvBool(csv, state.snapshot_mirror_published);
    csv << ',' << state.successful_polls
        << ',' << state.skipped_polls
        << ',';
    AppendCsvBool(csv, state.stale);
    csv << ',';
    AppendCsvString(csv, state.status_detail);
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
    csv << BuildCommonCsvPrefix(snapshot, "evidence-log") << ',';
    AppendCsvBool(csv, state.telemetry_available);
    csv << ',' << state.successful_polls
        << ',' << state.skipped_polls
        << ',';
    AppendCsvBool(csv, state.stale);
    csv << ',';
    AppendCsvString(csv, state.status_detail);
    csv << ',';
    AppendCsvBool(csv, state.sio_evidence_available);
    csv << ',';
    AppendCsvString(csv, state.sio_evidence_detail);
    csv << ',' << state.sio_voltages.size()
        << ',' << state.sio_temperatures.size();
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeFanTachEvidenceLogState* fan =
            FindFanTachEvidenceState(state.fan_tach_evidence, channel);
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->tach_hi_raw);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->tach_lo_raw);
        }
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioVoltageCount);
         ++index) {
        const RuntimeSioVoltageLogState* voltage =
            FindSioVoltageState(state.sio_voltages, index);
        csv << ',';
        AppendCsvBool(csv, voltage != nullptr);
        csv << ',';
        if (voltage != nullptr) {
            AppendCsvString(csv, voltage->label);
        }
        csv << ',';
        if (voltage != nullptr) {
            csv << static_cast<unsigned int>(voltage->raw);
        }
        csv << ',';
        if (voltage != nullptr) {
            AppendCsvDouble(csv, voltage->voltage_v, 4);
        }
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kRuntimeLogSioTemperatureCount);
         ++index) {
        const RuntimeSioTemperatureLogState* temperature =
            FindSioTemperatureState(state.sio_temperatures, index);
        csv << ',';
        AppendCsvBool(csv, temperature != nullptr);
        csv << ',';
        if (temperature != nullptr) {
            AppendCsvString(csv, temperature->label);
        }
        csv << ',';
        if (temperature != nullptr) {
            csv << static_cast<unsigned int>(temperature->raw);
        }
        csv << ',';
        if (temperature != nullptr) {
            csv << static_cast<unsigned int>(temperature->half_raw);
        }
        csv << ',';
        if (temperature != nullptr) {
            AppendCsvBool(csv, temperature->valid);
        }
        csv << ',';
        if (temperature != nullptr) {
            AppendCsvDouble(csv, temperature->temperature_c);
        }
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
           << ",process_private_bytes";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",channel" << channel << "_observed_temp_c"
               << ",channel" << channel << "_setpoint_pct"
               << ",channel" << channel << "_thermal_pressure_boost_pct"
               << ",channel" << channel << "_cpu_low_soak_boost_pct"
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
    csv << BuildCommonCsvPrefix(snapshot, "control-loop") << ',' << tick_count << ',';
    AppendCsvString(csv, timing.loop_started_wall_clock);
    csv << ',';
    AppendCsvString(csv, timing.loop_finished_wall_clock);
    csv << ',';
    AppendCsvDouble(csv, timing.loop_work_duration_ms);
    csv << ',' << timing.loop_intended_interval_ms << ',';
    AppendCsvDouble(csv, timing.loop_achieved_interval_ms);
    csv << ',';
    AppendCsvDouble(csv, timing.loop_slip_ms);
    csv << ',';
    AppendCsvBool(csv, timing.loop_overrun);
    csv << ',';
    AppendCsvDouble(csv, timing.process_cpu_delta_ms);
    csv << ',';
    AppendCsvDouble(csv, timing.process_cpu_pct);
    csv << ',' << timing.process_working_set_bytes
        << ',' << timing.process_private_bytes;
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeControlChannelLogState* state =
            FindChannelState(channels, channel);
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->observed_temp_c);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->setpoint_pct);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->thermal_pressure_boost_pct);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->cpu_low_soak_boost_pct);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvString(csv, state->response_source);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvString(csv, state->write_reason);
        }
        csv << ',';
        if (state != nullptr) {
            csv << state->total_writes;
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvBool(csv, state->write_active);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvBool(csv, state->baseline_captured);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->feedforward_pct);
        }
        csv << ',';
        if (state != nullptr &&
            !std::isnan(state->setpoint_pct) &&
            !std::isnan(state->feedforward_pct)) {
            AppendCsvDouble(csv, state->setpoint_pct - state->feedforward_pct);
        }
    }
    return csv.str();
}

}  // namespace svg_mb_control
