// Header/row alignment tests for the runtime CSV builders in
// src/runtime/runtime_csv_rows.cpp. The repeated per-index field groups
// (fan snapshot, fan tach evidence, SIO voltage/temperature, control
// channel) are descriptor-driven so the header and row come from one table;
// these tests lock the invariant that each builder's header column count
// equals its row field count, and spot-check the boundary column names.
//
// Field count and spot checks use a simple comma splitter. The fixtures use
// comma-free string values so no value is RFC 4180 quoted.

#include "runtime_csv_rows.h"
#include "runtime_snapshot.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

std::vector<std::string> SplitFields(const std::string& text) {
    std::vector<std::string> fields;
    std::size_t start = 0u;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            fields.push_back(text.substr(start));
            break;
        }
        fields.push_back(text.substr(start, comma - start));
        start = comma + 1u;
    }
    return fields;
}

void ExpectAligned(const std::string& header, const std::string& row,
                   const std::string& label) {
    const std::size_t header_fields = SplitFields(header).size();
    const std::size_t row_fields = SplitFields(row).size();
    if (header_fields != row_fields) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " header has " << header_fields
                  << " columns but row has " << row_fields << " fields\n";
    }
}

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& label) {
    if (haystack.find(needle) == std::string::npos) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " missing \"" << needle << "\"\n";
    }
}

void ExpectField(const std::vector<std::string>& header,
                 const std::vector<std::string>& row,
                 const std::string& column,
                 const std::string& expected,
                 const std::string& label) {
    const auto found = std::find(header.begin(), header.end(), column);
    if (found == header.end()) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " missing column " << column
                  << '\n';
        return;
    }
    const std::size_t index =
        static_cast<std::size_t>(found - header.begin());
    if (index >= row.size()) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " column " << column
                  << " has no row field\n";
        return;
    }
    if (row[index] != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " column " << column
                  << " expected \"" << expected << "\" got \""
                  << row[index] << "\"\n";
    }
}

// A snapshot with a few present fan channels (0, 2, 6) and gaps, so both the
// present and absent column paths run. All string values are comma-free.
svg_mb_control::RuntimeSnapshot MakeSnapshot() {
    using namespace svg_mb_control;
    RuntimeSnapshot snapshot;
    snapshot.snapshot_time_iso = "2026-05-29T00:00:00.000";
    snapshot.policy_writes_enabled_present = true;
    snapshot.policy_writes_enabled = true;
    snapshot.gpu.available = true;
    snapshot.gpu.gpu_name = "SimGPU";
    snapshot.gpu.core_c = 60.0;
    // FEAT-0020 read-only GPU board power (control-loop CSV only). A fresh
    // nonzero NVML read: sample_id advanced, read timestamp stamped, source and
    // acquisition both "nvml".
    snapshot.gpu.power_sample_id = 4u;
    snapshot.gpu.power_time_ms = 1234.0;
    snapshot.gpu.power_mw = 275000.0;
    snapshot.gpu.power_source = "nvml";
    snapshot.gpu.power_acquisition = "nvml";
    for (std::uint32_t channel : {0u, 2u, 6u}) {
        RuntimeFanSnapshot& fan = UpsertRuntimeFanChannel(snapshot, channel);
        fan.channel = channel;
        fan.label = "fan" + std::to_string(channel);
        fan.rpm = static_cast<std::uint16_t>(1000u + channel);
        fan.tach_raw = static_cast<std::uint16_t>(200u + channel);
        fan.tach_valid = channel != 2u;
        fan.duty_raw = static_cast<std::uint8_t>(80u + channel);
        fan.duty_percent = 33.5;
        fan.mode_raw = 7u;
        fan.manual_override = channel == 2u;
        fan.write_allowed = true;
        fan.policy_blocked = channel == 6u;
        fan.effective_write_allowed = !fan.policy_blocked;
    }
    return snapshot;
}

void TestReadLoopAligned() {
    using namespace svg_mb_control;
    const RuntimeSnapshot snapshot = MakeSnapshot();
    RuntimeReadLoopLogState state;
    state.telemetry_available = true;
    state.successful_polls = 5u;
    state.status_detail = "ok";
    const std::string header = BuildReadLoopCsvHeader();
    const std::string row = BuildReadLoopCsvRow(snapshot, state);
    ExpectAligned(header, row, "read-loop");
    const std::vector<std::string> header_fields = SplitFields(header);
    const std::vector<std::string> row_fields = SplitFields(row);
    ExpectContains(header, "fan0_present", "read-loop header");
    ExpectContains(header, "fan6_effective_write_allowed", "read-loop header");
    ExpectContains(header, "status_detail", "read-loop header");
    ExpectField(header_fields, row_fields, "fan0_present", "true",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan0_duty_raw", "80",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan1_present", "false",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan1_label", "",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan2_tach_valid", "false",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan6_policy_blocked", "true",
                "read-loop row");
    ExpectField(header_fields, row_fields, "fan6_effective_write_allowed",
                "false", "read-loop row");
    ExpectField(header_fields, row_fields, "status_detail", "ok",
                "read-loop row");
}

void TestEvidenceLogAligned() {
    using namespace svg_mb_control;
    const RuntimeSnapshot snapshot = MakeSnapshot();
    RuntimeEvidenceLogState state;
    state.telemetry_available = true;
    state.status_detail = "ok";
    state.sio_evidence_available = true;
    state.sio_evidence_detail = "captured";
    {
        RuntimeFanTachEvidenceLogState tach;
        tach.channel = 2u;
        tach.tach_hi_raw = 4u;
        tach.tach_lo_raw = 18u;
        state.fan_tach_evidence.push_back(tach);
    }
    {
        RuntimeSioVoltageLogState voltage;
        voltage.index = 0u;
        voltage.label = "vcore";
        voltage.raw = 150u;
        voltage.voltage_v = 1.2;
        state.sio_voltages.push_back(voltage);
    }
    {
        RuntimeSioTemperatureLogState temperature;
        temperature.index = 0u;
        temperature.label = "siotemp0";
        temperature.raw = 61u;
        temperature.half_raw = 128u;
        temperature.valid = true;
        temperature.temperature_c = 61.5;
        state.sio_temperatures.push_back(temperature);
    }
    const std::string header = BuildEvidenceLogCsvHeader();
    const std::string row = BuildEvidenceLogCsvRow(snapshot, state);
    // Covers the fan-tach, SIO voltage/temperature, and (transitively) the
    // GPU evidence blocks in one alignment check.
    ExpectAligned(header, row, "evidence-log");
    const std::vector<std::string> header_fields = SplitFields(header);
    const std::vector<std::string> row_fields = SplitFields(row);
    ExpectContains(header, "fan0_tach_hi_raw", "evidence-log header");
    ExpectContains(header, "sio_voltage0_v", "evidence-log header");
    ExpectContains(header, "sio_voltage15_v", "evidence-log header");
    ExpectContains(header, "sio_temp0_c", "evidence-log header");
    ExpectContains(header, "sio_temp22_c", "evidence-log header");
    ExpectField(header_fields, row_fields, "fan0_tach_hi_raw", "",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "fan2_tach_hi_raw", "4",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_voltage0_present", "true",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_voltage0_v", "1.2000",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_voltage1_present", "false",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_voltage1_v", "",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_temp0_valid", "true",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_temp0_c", "61.500",
                "evidence-log row");
    ExpectField(header_fields, row_fields, "sio_temp1_valid", "",
                "evidence-log row");
}

void TestControlLoopAligned() {
    using namespace svg_mb_control;
    const RuntimeSnapshot snapshot = MakeSnapshot();
    RuntimeControlLoopTimingState timing;
    timing.loop_started_wall_clock = "2026-05-29T00:00:00.000";
    timing.loop_finished_wall_clock = "2026-05-29T00:00:00.050";
    timing.loop_intended_interval_ms = 50u;
    timing.system_cpu_idle_delta_ms = 400.0;
    timing.system_cpu_kernel_delta_ms = 1000.0;
    timing.system_cpu_user_delta_ms = 200.0;
    timing.system_cpu_processor_count = 8u;
    timing.system_cpu_busy_pct = 66.667;
    std::vector<RuntimeControlChannelLogState> channels;
    {
        RuntimeControlChannelLogState channel;
        channel.channel = 0u;
        channel.observed_temp_c = 65.0;
        channel.setpoint_pct = 40.0;
        channel.feedforward_pct = 38.0;  // correction_pct present (= 2.000)
        channel.stage_boost_pct[0] = 1.0;
        channel.primary_temp_source = "cpu";
        channel.response_source = "primary_curve";
        channel.write_reason = "interval";
        channel.total_writes = 3u;
        channel.write_active = true;
        channel.baseline_captured = true;
        channels.push_back(channel);
    }
    // Channels 1..6 absent: exercises the absent path, including the
    // correction_pct gate when setpoint/feedforward are NaN.
    const std::string header = BuildControlLoopCsvHeader();
    RuntimeSnapshotIndex snapshot_index;
    snapshot_index.Rebuild(snapshot);
    const std::string row =
        BuildControlLoopCsvRow(snapshot, snapshot_index, 7u, timing, channels);
    ExpectAligned(header, row, "control-loop");
    const std::vector<std::string> header_fields = SplitFields(header);
    const std::vector<std::string> row_fields = SplitFields(row);
    ExpectContains(header, "system_cpu_busy_pct", "control-loop header");
    ExpectField(header_fields, row_fields, "system_cpu_idle_delta_ms",
                "400.000", "control-loop row");
    ExpectField(header_fields, row_fields, "system_cpu_processor_count", "8",
                "control-loop row");
    ExpectField(header_fields, row_fields, "system_cpu_busy_pct", "66.667",
                "control-loop row");
    ExpectContains(header, "channel0_observed_temp_c", "control-loop header");
    ExpectContains(header, "channel0_thermal_pressure_boost_pct",
                   "control-loop header");
    ExpectContains(header, "channel0_correction_pct", "control-loop header");
    ExpectContains(header, "channel6_correction_pct", "control-loop header");
    ExpectField(header_fields, row_fields, "loop_tick_count", "7",
                "control-loop row");
    ExpectField(header_fields, row_fields, "channel0_observed_temp_c",
                "65.000", "control-loop row");
    ExpectField(header_fields, row_fields,
                "channel0_thermal_pressure_boost_pct", "1.000",
                "control-loop row");
    ExpectField(header_fields, row_fields, "channel0_primary_temp_source",
                "cpu", "control-loop row");
    ExpectField(header_fields, row_fields, "channel0_feedforward_pct",
                "38.000", "control-loop row");
    ExpectField(header_fields, row_fields, "channel0_correction_pct", "2.000",
                "control-loop row");
    ExpectField(header_fields, row_fields, "channel1_observed_temp_c", "",
                "control-loop row");
    ExpectField(header_fields, row_fields, "channel6_correction_pct", "",
                "control-loop row");
    // FEAT-0020 GPU board power columns (control-loop only; not in read-loop or
    // evidence-log headers). Header/row alignment is already covered by
    // ExpectAligned above; these lock the names, placement, and value format.
    ExpectContains(header, "gpu_power_sample_id", "control-loop header");
    ExpectContains(header, "gpu_power_time_ms", "control-loop header");
    ExpectContains(header, "gpu_power_mw", "control-loop header");
    ExpectContains(header, "gpu_power_source", "control-loop header");
    ExpectContains(header, "gpu_power_acquisition", "control-loop header");
    ExpectField(header_fields, row_fields, "gpu_power_sample_id", "4",
                "control-loop row");
    ExpectField(header_fields, row_fields, "gpu_power_time_ms", "1234.000",
                "control-loop row");
    ExpectField(header_fields, row_fields, "gpu_power_mw", "275000.000",
                "control-loop row");
    ExpectField(header_fields, row_fields, "gpu_power_source", "nvml",
                "control-loop row");
    ExpectField(header_fields, row_fields, "gpu_power_acquisition", "nvml",
                "control-loop row");
}

}  // namespace

int main() {
    TestReadLoopAligned();
    TestEvidenceLogAligned();
    TestControlLoopAligned();
    if (g_failures > 0) {
        std::cerr << g_failures << " csv_rows test failure(s)\n";
        return 1;
    }
    std::cout << "csv_rows tests OK\n";
    return 0;
}
