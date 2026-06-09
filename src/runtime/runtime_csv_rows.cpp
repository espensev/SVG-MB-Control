#include "runtime_csv_rows.h"

#include "csv_util.h"
#include "gpu_evidence_csv.h"
#include "runtime_artifacts.h"
#include "runtime_util.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

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

// --- Descriptor-driven repeated CSV field groups -------------------------
//
// Each repeated per-index block (fan snapshot, fan tach evidence, SIO
// voltage/temperature, control channel) used to list its column names in a
// header loop and its row values in a separate row loop, kept aligned by
// hand. The descriptor tables below put the column-name suffix and its row
// writer on one line, and the header/row builders both iterate the same
// table, so the two cannot drift. Column names and emitted values are
// byte-identical to the previous hand-written blocks; readers that bind by
// name (native analyze ingest, tools/eval_dashboard/dashboard.js) are
// unaffected.

// One CSV column in a repeated group. `suffix` is appended after the
// "<prefix><index>" stem to form the column name (e.g. "_present"); `append`
// writes the matching row value with its own leading comma, honoring
// `present` exactly as the previous hand-written row builders did.
template <class Entity>
struct CsvColumn {
    std::string_view suffix;
    void (*append)(std::ostringstream&, bool present, const Entity& entity);
};

// Emits ",<prefix><index><suffix>" for every column in `columns`.
template <class Entity, std::size_t N>
void AppendEntityColumns(std::ostringstream& header,
                         std::string_view prefix,
                         std::uint32_t index,
                         const std::array<CsvColumn<Entity>, N>& columns) {
    for (const auto& column : columns) {
        header << ',' << prefix << index << column.suffix;
    }
}

// Writes the row value for every column in `columns`.
template <class Entity, std::size_t N>
void AppendEntityFields(std::ostringstream& csv,
                        bool present,
                        const Entity& entity,
                        const std::array<CsvColumn<Entity>, N>& columns) {
    for (const auto& column : columns) {
        column.append(csv, present, entity);
    }
}

// Header side of a full repeated group: `count` indexed blocks of `columns`.
template <class Entity, std::size_t N>
void AppendIndexedColumns(std::ostringstream& header,
                          std::string_view prefix,
                          std::size_t count,
                          const std::array<CsvColumn<Entity>, N>& columns) {
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(count); ++index) {
        AppendEntityColumns(header, prefix, index, columns);
    }
}

// Row side of a full repeated group: looks up each index with `find` (which
// returns a pointer or nullptr) and writes its columns, emitting empty cells
// for absent indices exactly as the previous hand-written loops did.
template <class Entity, std::size_t N, class Finder>
void AppendIndexedFields(std::ostringstream& csv,
                         std::size_t count,
                         const Finder& find,
                         const std::array<CsvColumn<Entity>, N>& columns) {
    static const Entity kEmpty{};
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(count); ++index) {
        const Entity* found = find(index);
        const bool present = found != nullptr;
        AppendEntityFields(csv, present, present ? *found : kEmpty, columns);
    }
}

constexpr std::array<CsvColumn<RuntimeFanSnapshot>, 12> kFanSnapshotColumns{{
    {"_present",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot&) {
         AppendCsvFieldBool(csv, present);
     }},
    {"_label",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldStringIf(csv, present, fan.label);
     }},
    {"_rpm",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldIf(csv, present, fan.rpm);
     }},
    {"_tach_raw",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldIf(csv, present, fan.tach_raw);
     }},
    {"_tach_valid",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldBoolIf(csv, present, fan.tach_valid);
     }},
    {"_duty_raw",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldIf(csv, present, static_cast<unsigned int>(fan.duty_raw));
     }},
    {"_duty_pct",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldDoubleIf(csv, present, fan.duty_percent, 2);
     }},
    {"_mode_raw",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldIf(csv, present, static_cast<unsigned int>(fan.mode_raw));
     }},
    {"_manual_override",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldBoolIf(csv, present, fan.manual_override);
     }},
    {"_write_allowed",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldBoolIf(csv, present, fan.write_allowed);
     }},
    {"_policy_blocked",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldBoolIf(csv, present, fan.policy_blocked);
     }},
    {"_effective_write_allowed",
     [](std::ostringstream& csv, bool present, const RuntimeFanSnapshot& fan) {
         AppendCsvFieldBoolIf(csv, present, fan.effective_write_allowed);
     }},
}};

constexpr std::array<CsvColumn<RuntimeFanTachEvidenceLogState>, 2>
    kFanTachEvidenceColumns{{
        {"_tach_hi_raw",
         [](std::ostringstream& csv, bool present,
            const RuntimeFanTachEvidenceLogState& fan) {
             AppendCsvFieldIf(csv, present,
                              static_cast<unsigned int>(fan.tach_hi_raw));
         }},
        {"_tach_lo_raw",
         [](std::ostringstream& csv, bool present,
            const RuntimeFanTachEvidenceLogState& fan) {
             AppendCsvFieldIf(csv, present,
                              static_cast<unsigned int>(fan.tach_lo_raw));
         }},
    }};

constexpr std::array<CsvColumn<RuntimeSioVoltageLogState>, 4>
    kSioVoltageColumns{{
        {"_present",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioVoltageLogState&) {
             AppendCsvFieldBool(csv, present);
         }},
        {"_label",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioVoltageLogState& voltage) {
             AppendCsvFieldStringIf(csv, present, voltage.label);
         }},
        {"_raw",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioVoltageLogState& voltage) {
             AppendCsvFieldIf(csv, present,
                              static_cast<unsigned int>(voltage.raw));
         }},
        {"_v",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioVoltageLogState& voltage) {
             AppendCsvFieldDoubleIf(csv, present, voltage.voltage_v, 4);
         }},
    }};

constexpr std::array<CsvColumn<RuntimeSioTemperatureLogState>, 6>
    kSioTemperatureColumns{{
        {"_present",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState&) {
             AppendCsvFieldBool(csv, present);
         }},
        {"_label",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState& temperature) {
             AppendCsvFieldStringIf(csv, present, temperature.label);
         }},
        {"_raw",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState& temperature) {
             AppendCsvFieldIf(csv, present,
                              static_cast<unsigned int>(temperature.raw));
         }},
        {"_half_raw",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState& temperature) {
             AppendCsvFieldIf(csv, present,
                              static_cast<unsigned int>(temperature.half_raw));
         }},
        {"_valid",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState& temperature) {
             AppendCsvFieldBoolIf(csv, present, temperature.valid);
         }},
        {"_c",
         [](std::ostringstream& csv, bool present,
            const RuntimeSioTemperatureLogState& temperature) {
             AppendCsvFieldDoubleIf(csv, present, temperature.temperature_c);
         }},
    }};

// Control-channel columns split around the boost-stage block, whose names
// come from kBoostStageSpecs and are emitted by the header/row builders
// directly (already a single source of truth).
constexpr std::array<CsvColumn<RuntimeControlChannelLogState>, 2>
    kChannelPreBoostColumns{{
        {"_observed_temp_c",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present, state.observed_temp_c);
         }},
        {"_setpoint_pct",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present, state.setpoint_pct);
         }},
    }};

constexpr std::array<CsvColumn<RuntimeControlChannelLogState>, 13>
    kChannelPostBoostColumns{{
        {"_low_band_stage_boost_pct",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present,
                                    state.low_band_stage_boost_pct);
         }},
        {"_low_band_effective_boost_pct",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present,
                                    state.low_band_effective_boost_pct);
         }},
        {"_low_band_debt",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present, state.low_band_debt);
         }},
        {"_low_band_signal",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present, state.low_band_signal);
         }},
        {"_low_band_stage_active",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldBoolIf(csv, present, state.low_band_stage_active);
         }},
        {"_primary_temp_source",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldStringIf(csv, present, state.primary_temp_source);
         }},
        {"_response_source",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldStringIf(csv, present, state.response_source);
         }},
        {"_write_reason",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldStringIf(csv, present, state.write_reason);
         }},
        {"_total_writes",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldIf(csv, present, state.total_writes);
         }},
        {"_write_active",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldBoolIf(csv, present, state.write_active);
         }},
        {"_baseline_captured",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldBoolIf(csv, present, state.baseline_captured);
         }},
        {"_feedforward_pct",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             AppendCsvFieldDoubleIf(csv, present, state.feedforward_pct);
         }},
        {"_correction_pct",
         [](std::ostringstream& csv, bool present,
            const RuntimeControlChannelLogState& state) {
             const bool correction_present =
                 present && !std::isnan(state.setpoint_pct) &&
                 !std::isnan(state.feedforward_pct);
             AppendCsvFieldDoubleIf(csv, correction_present,
                                    state.setpoint_pct - state.feedforward_pct);
         }},
    }};

std::string BuildCommonCsvHeader() {
    std::ostringstream header;
    header
        << "wall_clock,mode,snapshot_time,snapshot_age_ms,"
        << "amd_sensor_count,amd_sensor_summary,cpu_tctl_c,cpu_max_c,"
        << "gpu_available,gpu_name,gpu_last_warning,"
        << "gpu_core_c,gpu_memjn_c,gpu_hotspot_c,"
        << "fan_count,policy_writes_enabled_present,policy_writes_enabled";
    AppendIndexedColumns(header, "fan", kRuntimeLogFanChannelCount,
                         kFanSnapshotColumns);
    return header.str();
}

// Writes the shared leading CSV columns directly into the caller's stream.
// Previously this built its own std::ostringstream and returned a heap
// std::string that the row builder then copied in; writing in place removes
// one ostringstream + one string allocation/copy per row.
void BuildCommonCsvPrefix(std::ostringstream& csv,
                          const RuntimeSnapshot& snapshot,
                          const RuntimeSnapshotIndex& snapshot_index,
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
                         snapshot_index.FindAmdSensorTemperature("Tctl/Tdie"));
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

    AppendIndexedFields(
        csv, kRuntimeLogFanChannelCount,
        [&](std::uint32_t channel) {
            return snapshot_index.FindFanChannel(channel);
        },
        kFanSnapshotColumns);
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
    RuntimeSnapshotIndex snapshot_index;
    snapshot_index.Rebuild(snapshot);
    BuildCommonCsvPrefix(csv, snapshot, snapshot_index, "read-loop");
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
    AppendIndexedColumns(header, "fan", kRuntimeLogFanChannelCount,
                         kFanTachEvidenceColumns);
    AppendIndexedColumns(header, "sio_voltage", kRuntimeLogSioVoltageCount,
                         kSioVoltageColumns);
    AppendIndexedColumns(header, "sio_temp", kRuntimeLogSioTemperatureCount,
                         kSioTemperatureColumns);
    header << BuildGpuEvidenceCsvHeader();
    return header.str();
}

std::string BuildEvidenceLogCsvRow(const RuntimeSnapshot& snapshot,
                                   const RuntimeEvidenceLogState& state) {
    std::ostringstream csv;
    RuntimeSnapshotIndex snapshot_index;
    snapshot_index.Rebuild(snapshot);
    BuildCommonCsvPrefix(csv, snapshot, snapshot_index, "evidence-log");
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

    AppendIndexedFields(
        csv, kRuntimeLogFanChannelCount,
        [&](std::uint32_t channel) {
            return FindFanTachEvidenceState(state.fan_tach_evidence, channel);
        },
        kFanTachEvidenceColumns);
    AppendIndexedFields(
        csv, kRuntimeLogSioVoltageCount,
        [&](std::uint32_t index) {
            return FindSioVoltageState(state.sio_voltages, index);
        },
        kSioVoltageColumns);
    AppendIndexedFields(
        csv, kRuntimeLogSioTemperatureCount,
        [&](std::uint32_t index) {
            return FindSioTemperatureState(state.sio_temperatures, index);
        },
        kSioTemperatureColumns);
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
           << ",system_cpu_idle_delta_ms"
           << ",system_cpu_kernel_delta_ms"
           << ",system_cpu_user_delta_ms"
           << ",system_cpu_processor_count"
           << ",system_cpu_busy_pct"
           << ",cadence_transient"
           << ",cpu_power_sample_id"
           << ",cpu_power_window_ms"
           << ",cpu_pkg_energy_delta_uj"
           << ",cpu_pkg_energy_acquisition";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        // Canonical control-loop CSV header. The pre/post column names come
        // from kChannelPreBoostColumns/kChannelPostBoostColumns (shared with
        // the row builder); the boost column names come from kBoostStageSpecs.
        // CSV readers that bind these names by string (native analyze ingest
        // and tools/eval_dashboard/dashboard.js) track this list.
        AppendEntityColumns(header, "channel", channel,
                            kChannelPreBoostColumns);
        for (std::size_t s = 0; s < kBoostStageCount; ++s) {
            header << ",channel" << channel << "_"
                   << kBoostStageSpecs[s].name << "_boost_pct";
        }
        AppendEntityColumns(header, "channel", channel,
                            kChannelPostBoostColumns);
    }
    return header.str();
}

std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    const RuntimeSnapshotIndex& snapshot_index,
    std::uint64_t tick_count,
    const RuntimeControlLoopTimingState& timing,
    const std::vector<RuntimeControlChannelLogState>& channels) {
    std::ostringstream csv;
    BuildCommonCsvPrefix(csv, snapshot, snapshot_index, "control-loop");
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
    AppendCsvFieldDouble(csv, timing.system_cpu_idle_delta_ms);
    AppendCsvFieldDouble(csv, timing.system_cpu_kernel_delta_ms);
    AppendCsvFieldDouble(csv, timing.system_cpu_user_delta_ms);
    AppendCsvFieldIf(csv, timing.system_cpu_processor_count != 0u,
                     timing.system_cpu_processor_count);
    AppendCsvFieldDouble(csv, timing.system_cpu_busy_pct);
    AppendCsvFieldDouble(csv, timing.cadence_transient);
    // FEAT-0006 read-only RAPL package-energy evidence (from the snapshot, not
    // the timing block). sample_id blank when 0 (no sample yet); window/delta
    // blank (NaN) when unavailable or guard-blanked; acquisition is always a
    // state string.
    AppendCsvFieldIf(csv, snapshot.pkg_energy_sample_id != 0u,
                     snapshot.pkg_energy_sample_id);
    AppendCsvFieldDouble(csv, snapshot.pkg_energy_window_ms);
    AppendCsvFieldDouble(csv, snapshot.pkg_energy_delta_uj);
    AppendCsvFieldString(csv, snapshot.pkg_energy_acquisition);

    static const RuntimeControlChannelLogState kEmptyChannel{};
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeControlChannelLogState* found =
            FindChannelState(channels, channel);
        const bool present = found != nullptr;
        const RuntimeControlChannelLogState& state =
            present ? *found : kEmptyChannel;
        AppendEntityFields(csv, present, state, kChannelPreBoostColumns);
        for (std::size_t s = 0; s < kBoostStageCount; ++s) {
            AppendCsvFieldDoubleIf(csv, present, state.stage_boost_pct[s]);
        }
        AppendEntityFields(csv, present, state, kChannelPostBoostColumns);
    }
    return csv.str();
}

std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    std::uint64_t tick_count,
    const RuntimeControlLoopTimingState& timing,
    const std::vector<RuntimeControlChannelLogState>& channels) {
    RuntimeSnapshotIndex snapshot_index;
    snapshot_index.Rebuild(snapshot);
    return BuildControlLoopCsvRow(snapshot, snapshot_index, tick_count,
                                  timing, channels);
}

}  // namespace svg_mb_control
