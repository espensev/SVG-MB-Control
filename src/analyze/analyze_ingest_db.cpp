#include "analyze_ingest_db.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::analyze {

namespace {

bool IsStartEvent(const std::string& event_type) {
    return event_type == "control_loop.start" ||
           event_type == "read_loop.start" ||
           event_type == "write_orchestrator.start";
}

}  // namespace

bool IsManifestPathInDb(Database& db, const std::string& canonical_path) {
    Statement stmt = db.Prepare(
        "SELECT 1 FROM runs WHERE manifest_path = ?1 LIMIT 1");
    stmt.BindText(1, canonical_path);
    return stmt.Step();
}

void DeleteRunByManifestPath(Database& db, const std::string& canonical_path) {
    Statement stmt = db.Prepare(
        "DELETE FROM runs WHERE manifest_path = ?1");
    stmt.BindText(1, canonical_path);
    stmt.Step();
}

bool IsSessionInDb(Database& db,
                   const std::string& session_start,
                   const std::string& mode) {
    Statement stmt = db.Prepare(
        "SELECT 1 FROM runs WHERE session_start = ?1 AND mode = ?2 LIMIT 1");
    stmt.BindText(1, session_start);
    stmt.BindText(2, mode);
    return stmt.Step();
}

void DeleteRunBySession(Database& db,
                        const std::string& session_start,
                        const std::string& mode) {
    Statement stmt = db.Prepare(
        "DELETE FROM runs WHERE session_start = ?1 AND mode = ?2");
    stmt.BindText(1, session_start);
    stmt.BindText(2, mode);
    stmt.Step();
}

bool IsCapturePathInDb(Database& db, const std::string& canonical_path) {
    Statement stmt = db.Prepare(
        "SELECT 1 FROM plant_model_captures WHERE capture_path = ?1 LIMIT 1");
    stmt.BindText(1, canonical_path);
    return stmt.Step();
}

void DeleteCaptureByPath(Database& db, const std::string& canonical_path) {
    Statement stmt = db.Prepare(
        "DELETE FROM plant_model_captures WHERE capture_path = ?1");
    stmt.BindText(1, canonical_path);
    stmt.Step();
}

std::int64_t InsertRun(Database& db,
                       const std::string& manifest_path_canonical,
                       const std::filesystem::path& csv_path,
                       const ManifestData& manifest,
                       const std::string& ingested_at) {
    Statement stmt = db.Prepare(
        "INSERT INTO runs("
        "manifest_path, csv_archive_path, session_start, mode, status,"
        "tool_version, git_hash, row_count_declared, event_count_declared,"
        "csv_flush_policy, mirror_mode, last_update, ingested_at"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)");
    stmt.BindText(1, manifest_path_canonical);
    if (csv_path.empty()) {
        stmt.BindNull(2);
    } else {
        stmt.BindText(2, csv_path.string());
    }
    stmt.BindText(3, manifest.session_start);
    stmt.BindText(4, manifest.mode);
    stmt.BindText(5, manifest.status);
    stmt.BindOptionalText(6, manifest.tool_version);
    stmt.BindOptionalText(7, manifest.git_hash);
    stmt.BindOptionalInt(8, manifest.row_count);
    stmt.BindOptionalInt(9, manifest.event_count);
    stmt.BindOptionalText(10, manifest.csv_flush_policy);
    stmt.BindOptionalText(11, manifest.mirror_mode);
    stmt.BindOptionalText(12, manifest.last_update);
    stmt.BindText(13, ingested_at);
    stmt.Step();
    return db.LastInsertRowId();
}

void UpdateRunIngestCounts(Database& db,
                           std::int64_t run_id,
                           int row_count,
                           int event_count) {
    Statement stmt = db.Prepare(
        "UPDATE runs SET row_count_ingested = ?1, "
        "event_count_ingested = ?2 WHERE id = ?3");
    stmt.BindInt(1, row_count);
    stmt.BindInt(2, event_count);
    stmt.BindInt(3, run_id);
    stmt.Step();
}

void InsertTickRows(Database& db,
                    std::int64_t run_id,
                    const std::vector<ParsedTickRow>& rows) {
    Statement tick = db.Prepare(
        "INSERT INTO tick_samples("
        "run_id, tick_count, wall_clock, mode, snapshot_time, snapshot_age_ms,"
        "amd_sensor_count, amd_sensor_summary, cpu_tctl_c, cpu_max_c,"
        "gpu_available, gpu_name, gpu_last_warning,"
        "gpu_core_c, gpu_memjn_c, gpu_hotspot_c, gpu_envelope_c,"
        "fan_count, policy_writes_enabled_present, policy_writes_enabled,"
        "loop_started_wall_clock, loop_finished_wall_clock,"
        "loop_work_duration_ms, loop_intended_interval_ms,"
        "loop_achieved_interval_ms, loop_slip_ms, loop_overrun,"
        "process_cpu_delta_ms, process_cpu_pct,"
        "process_working_set_bytes, process_private_bytes,"
        "cadence_transient"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
        "?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32)");

    Statement fan = db.Prepare(
        "INSERT INTO tick_fan_samples("
        "run_id, tick_count, fan_index, present, label, rpm, tach_raw,"
        "tach_valid, duty_raw, duty_pct, mode_raw, manual_override,"
        "write_allowed, policy_blocked, effective_write_allowed"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");

    Statement ch = db.Prepare(
        "INSERT INTO tick_channel_samples("
        "run_id, tick_count, channel, observed_temp_c, setpoint_pct,"
        "thermal_pressure_boost_pct, midband_pressure_boost_pct,"
        "gpu_airflow_boost_pct, cpu_low_soak_boost_pct,"
        "response_source, write_reason, total_writes, write_active,"
        "baseline_captured, feedforward_pct, correction_pct"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)");

    for (const auto& row : rows) {
        tick.BindInt(1, run_id);
        tick.BindInt(2, row.tick_count);
        tick.BindText(3, row.wall_clock);
        tick.BindOptionalText(4, row.mode);
        tick.BindOptionalText(5, row.snapshot_time);
        tick.BindOptionalInt(6, row.snapshot_age_ms);
        tick.BindOptionalInt(7, row.amd_sensor_count);
        tick.BindOptionalText(8, row.amd_sensor_summary);
        tick.BindOptionalDouble(9, row.cpu_tctl_c);
        tick.BindOptionalDouble(10, row.cpu_max_c);
        tick.BindOptionalInt(11, row.gpu_available);
        tick.BindOptionalText(12, row.gpu_name);
        tick.BindOptionalText(13, row.gpu_last_warning);
        tick.BindOptionalDouble(14, row.gpu_core_c);
        tick.BindOptionalDouble(15, row.gpu_memjn_c);
        tick.BindOptionalDouble(16, row.gpu_hotspot_c);
        tick.BindOptionalDouble(17, row.gpu_envelope_c);
        tick.BindOptionalInt(18, row.fan_count);
        tick.BindOptionalInt(19, row.policy_writes_enabled_present);
        tick.BindOptionalInt(20, row.policy_writes_enabled);
        tick.BindOptionalText(21, row.loop_started_wall_clock);
        tick.BindOptionalText(22, row.loop_finished_wall_clock);
        tick.BindOptionalDouble(23, row.loop_work_duration_ms);
        tick.BindOptionalInt(24, row.loop_intended_interval_ms);
        tick.BindOptionalDouble(25, row.loop_achieved_interval_ms);
        tick.BindOptionalDouble(26, row.loop_slip_ms);
        tick.BindOptionalInt(27, row.loop_overrun);
        tick.BindOptionalDouble(28, row.process_cpu_delta_ms);
        tick.BindOptionalDouble(29, row.process_cpu_pct);
        tick.BindOptionalInt(30, row.process_working_set_bytes);
        tick.BindOptionalInt(31, row.process_private_bytes);
        tick.BindOptionalDouble(32, row.cadence_transient);
        tick.Step();
        tick.Reset();

        for (const auto& f : row.fans) {
            fan.BindInt(1, run_id);
            fan.BindInt(2, row.tick_count);
            fan.BindInt(3, f.fan_index);
            fan.BindOptionalInt(4, f.present);
            fan.BindOptionalText(5, f.label);
            fan.BindOptionalInt(6, f.rpm);
            fan.BindOptionalInt(7, f.tach_raw);
            fan.BindOptionalInt(8, f.tach_valid);
            fan.BindOptionalInt(9, f.duty_raw);
            fan.BindOptionalDouble(10, f.duty_pct);
            fan.BindOptionalInt(11, f.mode_raw);
            fan.BindOptionalInt(12, f.manual_override);
            fan.BindOptionalInt(13, f.write_allowed);
            fan.BindOptionalInt(14, f.policy_blocked);
            fan.BindOptionalInt(15, f.effective_write_allowed);
            fan.Step();
            fan.Reset();
        }

        for (const auto& c : row.channels) {
            ch.BindInt(1, run_id);
            ch.BindInt(2, row.tick_count);
            ch.BindInt(3, c.channel);
            ch.BindOptionalDouble(4, c.observed_temp_c);
            ch.BindOptionalDouble(5, c.setpoint_pct);
            ch.BindOptionalDouble(6, c.thermal_pressure_boost_pct);
            ch.BindOptionalDouble(7, c.midband_pressure_boost_pct);
            ch.BindOptionalDouble(8, c.gpu_airflow_boost_pct);
            ch.BindOptionalDouble(9, c.cpu_low_soak_boost_pct);
            ch.BindOptionalText(10, c.response_source);
            ch.BindOptionalText(11, c.write_reason);
            ch.BindOptionalInt(12, c.total_writes);
            ch.BindOptionalInt(13, c.write_active);
            ch.BindOptionalInt(14, c.baseline_captured);
            ch.BindOptionalDouble(15, c.feedforward_pct);
            ch.BindOptionalDouble(16, c.correction_pct);
            ch.Step();
            ch.Reset();
        }
    }
}

int InsertEventsAttributed(Database& db,
                           const std::vector<EventData>& events,
                           const std::vector<RunWindow>& runs) {
    std::map<std::string, std::int64_t> start_to_run;
    for (const auto& r : runs) {
        start_to_run.emplace(r.session_start, r.run_id);
    }

    Statement insert = db.Prepare(
        "INSERT INTO events("
        "run_id, event_time, event_type, mode, success, channel,"
        "setpoint_pct, observed_temp_c, tick_count, detail, extra_json"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");

    std::optional<std::int64_t> current_run;
    int inserted = 0;
    for (const auto& e : events) {
        if (IsStartEvent(e.event_type)) {
            auto it = start_to_run.find(e.event_time);
            if (it != start_to_run.end()) {
                current_run = it->second;
            } else {
                current_run.reset();
            }
        }
        if (current_run.has_value()) {
            insert.BindInt(1, *current_run);
        } else {
            insert.BindNull(1);
        }
        insert.BindText(2, e.event_time);
        insert.BindText(3, e.event_type);
        insert.BindOptionalText(4, e.mode);
        insert.BindOptionalInt(5, e.success);
        insert.BindOptionalInt(6, e.channel);
        insert.BindOptionalDouble(7, e.setpoint_pct);
        insert.BindOptionalDouble(8, e.observed_temp_c);
        insert.BindOptionalInt(9, e.tick_count);
        insert.BindOptionalText(10, e.detail);
        if (e.extra_json.empty()) {
            insert.BindNull(11);
        } else {
            insert.BindText(11, e.extra_json);
        }
        insert.Step();
        insert.Reset();
        ++inserted;
    }
    return inserted;
}

std::int64_t InsertPlantModelCapture(Database& db,
                                     const std::string& path_canonical,
                                     const PlantModelData& data,
                                     const std::string& ingested_at) {
    Statement stmt = db.Prepare(
        "INSERT INTO plant_model_captures("
        "capture_path, captured_local, abort_reason, settle_window_ms,"
        "abort_temp_ceiling_c, tool_version, git_hash, sequence_json,"
        "ingested_at"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    stmt.BindText(1, path_canonical);
    stmt.BindText(2, data.captured_local);
    stmt.BindOptionalText(3, data.abort_reason);
    stmt.BindInt(4, data.settle_window_ms);
    stmt.BindDouble(5, data.abort_temp_ceiling_c);
    stmt.BindOptionalText(6, data.tool_version);
    stmt.BindOptionalText(7, data.git_hash);
    stmt.BindText(8, data.sequence_json);
    stmt.BindText(9, ingested_at);
    stmt.Step();
    return db.LastInsertRowId();
}

void InsertPlantModelChannelsAndSteps(Database& db,
                                       std::int64_t capture_id,
                                       const PlantModelData& data) {
    Statement ch = db.Prepare(
        "INSERT INTO plant_model_channels("
        "capture_id, channel, baseline_captured, restored,"
        "baseline_duty_raw, baseline_mode_raw, baseline_rpm,"
        "baseline_tctl_c, baseline_gpu_envelope_c, note"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");

    Statement step = db.Prepare(
        "INSERT INTO plant_model_steps("
        "capture_id, channel, step_index, duty_pct_target, hold_ms,"
        "settle_window_ms, settle_sample_count, duty_pct_observed_mean,"
        "rpm_mean, rpm_stddev, tctl_c_mean, gpu_envelope_c_mean"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");

    for (const auto& c : data.channels) {
        ch.BindInt(1, capture_id);
        ch.BindInt(2, c.channel);
        ch.BindOptionalInt(3, c.baseline_captured);
        ch.BindOptionalInt(4, c.restored);
        ch.BindOptionalInt(5, c.baseline_duty_raw);
        ch.BindOptionalInt(6, c.baseline_mode_raw);
        ch.BindOptionalDouble(7, c.baseline_rpm);
        ch.BindOptionalDouble(8, c.baseline_tctl_c);
        ch.BindOptionalDouble(9, c.baseline_gpu_envelope_c);
        ch.BindOptionalText(10, c.note);
        ch.Step();
        ch.Reset();

        for (const auto& s : c.steps) {
            step.BindInt(1, capture_id);
            step.BindInt(2, c.channel);
            step.BindInt(3, s.step_index);
            step.BindDouble(4, s.duty_pct_target);
            step.BindInt(5, s.hold_ms);
            step.BindInt(6, s.settle_window_ms);
            step.BindOptionalInt(7, s.settle_sample_count);
            step.BindOptionalDouble(8, s.duty_pct_observed_mean);
            step.BindOptionalDouble(9, s.rpm_mean);
            step.BindOptionalDouble(10, s.rpm_stddev);
            step.BindOptionalDouble(11, s.tctl_c_mean);
            step.BindOptionalDouble(12, s.gpu_envelope_c_mean);
            step.Step();
            step.Reset();
        }
    }
}

}  // namespace svg_mb_control::analyze
