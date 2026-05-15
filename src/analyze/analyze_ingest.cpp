#include "analyze_ingest.h"

#include "analyze_csv.h"
#include "analyze_db.h"
#include "analyze_json_artifacts.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace svg_mb_control::analyze {

namespace {

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &tt);
#else
    localtime_r(&tt, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%FT%T");
    return oss.str();
}

std::filesystem::path Canonicalize(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return std::filesystem::absolute(path).lexically_normal();
    }
    return canonical;
}

std::filesystem::path ResolvePairedCsv(
    const std::filesystem::path& manifest_path,
    const ManifestData& manifest) {
    if (manifest.csv_archive_path.has_value()) {
        std::error_code ec;
        if (std::filesystem::exists(*manifest.csv_archive_path, ec)) {
            return *manifest.csv_archive_path;
        }
    }
    const std::filesystem::path parent = manifest_path.parent_path();
    const std::string manifest_name = manifest_path.filename().string();
    const std::string suffix_a = ".manifest.json";
    const std::string suffix_b = "_manifest.json";
    if (manifest_name.size() > suffix_a.size() &&
        manifest_name.compare(manifest_name.size() - suffix_a.size(),
                              suffix_a.size(), suffix_a) == 0) {
        const std::string stem = manifest_name.substr(
            0, manifest_name.size() - suffix_a.size());
        const auto candidate = parent / (stem + ".csv");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    if (manifest_name.size() > suffix_b.size() &&
        manifest_name.compare(manifest_name.size() - suffix_b.size(),
                              suffix_b.size(), suffix_b) == 0) {
        const std::string stem = manifest_name.substr(
            0, manifest_name.size() - suffix_b.size());
        const auto candidate = parent / (stem.substr(
            0, stem.find_last_of('_')) + "_output.csv");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    if (manifest_name == "svg_mb_control_manifest.json") {
        const auto candidate = parent / "svg_mb_control_output.csv";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

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
        "gpu_core_c, gpu_memjn_c, gpu_hotspot_c,"
        "fan_count, policy_writes_enabled_present, policy_writes_enabled,"
        "loop_started_wall_clock, loop_finished_wall_clock,"
        "loop_work_duration_ms, loop_intended_interval_ms,"
        "loop_achieved_interval_ms, loop_slip_ms, loop_overrun,"
        "process_cpu_delta_ms, process_cpu_pct,"
        "process_working_set_bytes, process_private_bytes"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
        "?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30)");

    Statement fan = db.Prepare(
        "INSERT INTO tick_fan_samples("
        "run_id, tick_count, fan_index, present, label, rpm, tach_raw,"
        "tach_valid, duty_raw, duty_pct, mode_raw, manual_override,"
        "write_allowed, policy_blocked, effective_write_allowed"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");

    Statement ch = db.Prepare(
        "INSERT INTO tick_channel_samples("
        "run_id, tick_count, channel, observed_temp_c, setpoint_pct,"
        "thermal_pressure_boost_pct, total_writes, write_active,"
        "baseline_captured, feedforward_pct, correction_pct"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");

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
        tick.BindOptionalInt(17, row.fan_count);
        tick.BindOptionalInt(18, row.policy_writes_enabled_present);
        tick.BindOptionalInt(19, row.policy_writes_enabled);
        tick.BindOptionalText(20, row.loop_started_wall_clock);
        tick.BindOptionalText(21, row.loop_finished_wall_clock);
        tick.BindOptionalDouble(22, row.loop_work_duration_ms);
        tick.BindOptionalInt(23, row.loop_intended_interval_ms);
        tick.BindOptionalDouble(24, row.loop_achieved_interval_ms);
        tick.BindOptionalDouble(25, row.loop_slip_ms);
        tick.BindOptionalInt(26, row.loop_overrun);
        tick.BindOptionalDouble(27, row.process_cpu_delta_ms);
        tick.BindOptionalDouble(28, row.process_cpu_pct);
        tick.BindOptionalInt(29, row.process_working_set_bytes);
        tick.BindOptionalInt(30, row.process_private_bytes);
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
            ch.BindOptionalInt(7, c.total_writes);
            ch.BindOptionalInt(8, c.write_active);
            ch.BindOptionalInt(9, c.baseline_captured);
            ch.BindOptionalDouble(10, c.feedforward_pct);
            ch.BindOptionalDouble(11, c.correction_pct);
            ch.Step();
            ch.Reset();
        }
    }
}

struct RunWindow {
    std::int64_t run_id;
    std::string session_start;
    std::string mode;
};

bool IsStartEvent(const std::string& event_type) {
    return event_type == "control_loop.start" ||
           event_type == "read_loop.start" ||
           event_type == "write_orchestrator.start";
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

std::vector<std::filesystem::path> CollectManifestPaths(
    const std::filesystem::path& runtime_home) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path logs = runtime_home / "logs";
    std::error_code ec;
    if (!std::filesystem::is_directory(logs, ec)) {
        return out;
    }
    const auto live = logs / "svg_mb_control_manifest.json";
    if (std::filesystem::exists(live, ec)) {
        out.push_back(live);
    }
    const auto archive = logs / "archive";
    if (std::filesystem::is_directory(archive, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(archive, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            const std::string suffix = ".manifest.json";
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(),
                             suffix.size(), suffix) == 0) {
                out.push_back(entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::filesystem::path> CollectPlantModelPaths(
    const std::filesystem::path& runtime_home) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const auto live = runtime_home / "plant_model.json";
    if (std::filesystem::exists(live, ec)) {
        out.push_back(live);
    }
    if (std::filesystem::is_directory(runtime_home, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(runtime_home, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name == "plant_model.json") {
                continue;
            }
            const std::string prefix = "plant_model_";
            const std::string suffix = ".json";
            if (name.size() > prefix.size() + suffix.size() &&
                name.compare(0, prefix.size(), prefix) == 0 &&
                name.compare(name.size() - suffix.size(),
                             suffix.size(), suffix) == 0) {
                out.push_back(entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int RunAnalyzeIngest(const IngestOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.runtime_home, ec)) {
        std::cerr << "Error: runtime_home is not a directory: "
                  << options.runtime_home.string() << '\n';
        return 1;
    }

    const std::filesystem::path db_path = options.db_path.empty()
        ? options.runtime_home / "svg_mb_control.db"
        : options.db_path;

    if (!db_path.parent_path().empty()) {
        std::filesystem::create_directories(db_path.parent_path(), ec);
    }

    Database db;
    try {
        db.Open(db_path);
    } catch (const std::exception& error) {
        std::cerr << "Error: failed to open database " << db_path.string()
                  << ": " << error.what() << '\n';
        return 1;
    }

    try {
        BootstrapSchema(db);
    } catch (const std::exception& error) {
        std::cerr << "Error: schema bootstrap failed: " << error.what() << '\n';
        return 1;
    }

    const int version = GetSchemaVersion(db);
    if (version != kSchemaVersion) {
        std::cerr << "Error: database schema version " << version
                  << " is not supported (this build expects "
                  << kSchemaVersion << ")\n";
        return 1;
    }

    const std::string ingested_at =
        FormatLocalIso8601(std::chrono::system_clock::now());

    IngestSummary summary;
    std::vector<RunWindow> run_windows;

    const auto manifests = CollectManifestPaths(options.runtime_home);
    for (const auto& manifest_path : manifests) {
        ManifestData manifest;
        try {
            manifest = ParseRuntimeManifest(manifest_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: skipping manifest "
                      << manifest_path.string() << ": " << error.what()
                      << '\n';
            continue;
        }
        const std::string canonical = Canonicalize(manifest_path).string();
        const bool manifest_seen = IsManifestPathInDb(db, canonical);
        const bool session_seen = IsSessionInDb(
            db, manifest.session_start, manifest.mode);
        if (manifest_seen || session_seen) {
            if (!options.force) {
                ++summary.runs_skipped;
                continue;
            }
            Transaction txn(db.handle());
            if (manifest_seen) {
                DeleteRunByManifestPath(db, canonical);
            }
            if (session_seen) {
                DeleteRunBySession(db, manifest.session_start, manifest.mode);
            }
            txn.Commit();
        }

        const std::filesystem::path csv_path =
            ResolvePairedCsv(manifest_path, manifest);
        ParsedCsv parsed_csv;
        if (!csv_path.empty()) {
            try {
                parsed_csv = ParseControlLoopCsv(csv_path);
            } catch (const std::exception& error) {
                std::cerr << "Warning: failed to parse CSV "
                          << csv_path.string() << ": " << error.what()
                          << '\n';
            }
        } else if (!options.quiet) {
            std::cerr << "Warning: no paired CSV found for "
                      << manifest_path.string() << '\n';
        }

        std::int64_t run_id = 0;
        try {
            Transaction txn(db.handle());
            run_id = InsertRun(db, canonical, csv_path, manifest, ingested_at);
            InsertTickRows(db, run_id, parsed_csv.rows);
            UpdateRunIngestCounts(
                db, run_id,
                static_cast<int>(parsed_csv.rows.size()), 0);
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert run for "
                      << manifest_path.string() << ": " << error.what()
                      << '\n';
            continue;
        }

        run_windows.push_back({run_id, manifest.session_start, manifest.mode});
        summary.tick_samples += static_cast<int>(parsed_csv.rows.size());
        ++summary.runs_ingested;

        if (!options.quiet) {
            std::cout << "ingested run id=" << run_id
                      << " session=" << manifest.session_start
                      << " ticks=" << parsed_csv.rows.size() << '\n';
        }
    }

    const std::filesystem::path events_path =
        options.runtime_home / "logs" / "svg_mb_control_events.jsonl";
    if (std::filesystem::exists(events_path, ec)) {
        std::vector<EventData> events;
        try {
            events = ParseEventsJsonl(events_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: failed to parse events: "
                      << error.what() << '\n';
        }

        if (options.force) {
            Transaction txn(db.handle());
            db.Exec("DELETE FROM events");
            txn.Commit();
        }

        std::vector<RunWindow> windows_for_attribution;
        if (options.force) {
            Statement query = db.Prepare(
                "SELECT id, session_start, mode FROM runs ORDER BY id");
            while (query.Step()) {
                windows_for_attribution.push_back({
                    query.ColumnInt(0),
                    query.ColumnText(1),
                    query.ColumnText(2),
                });
            }
        } else {
            windows_for_attribution = run_windows;
        }

        try {
            Transaction txn(db.handle());
            const int n = InsertEventsAttributed(
                db, events, windows_for_attribution);
            summary.events_ingested += n;

            for (const auto& w : windows_for_attribution) {
                Statement count = db.Prepare(
                    "SELECT COUNT(*) FROM events WHERE run_id = ?1");
                count.BindInt(1, w.run_id);
                count.Step();
                Statement update = db.Prepare(
                    "UPDATE runs SET event_count_ingested = ?1 WHERE id = ?2");
                update.BindInt(1, count.ColumnInt(0));
                update.BindInt(2, w.run_id);
                update.Step();
            }
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert events: " << error.what()
                      << '\n';
        }
    }

    const auto plant_paths = CollectPlantModelPaths(options.runtime_home);
    for (const auto& pm_path : plant_paths) {
        const std::string canonical = Canonicalize(pm_path).string();
        if (IsCapturePathInDb(db, canonical)) {
            if (!options.force) {
                ++summary.plant_models_skipped;
                continue;
            }
            Transaction txn(db.handle());
            DeleteCaptureByPath(db, canonical);
            txn.Commit();
        }

        PlantModelData data;
        try {
            data = ParsePlantModelCapture(pm_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: failed to parse plant model "
                      << pm_path.string() << ": " << error.what() << '\n';
            continue;
        }

        try {
            Transaction txn(db.handle());
            const std::int64_t capture_id =
                InsertPlantModelCapture(db, canonical, data, ingested_at);
            InsertPlantModelChannelsAndSteps(db, capture_id, data);
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert plant model "
                      << pm_path.string() << ": " << error.what() << '\n';
            continue;
        }

        ++summary.plant_models_ingested;
        if (!options.quiet) {
            std::cout << "ingested plant_model " << pm_path.string()
                      << " channels=" << data.channels.size() << '\n';
        }
    }

    std::cout << "analyze ingest: db=" << db_path.string()
              << " runs_ingested=" << summary.runs_ingested
              << " runs_skipped=" << summary.runs_skipped
              << " tick_samples=" << summary.tick_samples
              << " events=" << summary.events_ingested
              << " plant_models=" << summary.plant_models_ingested
              << " plant_models_skipped=" << summary.plant_models_skipped
              << '\n';
    return 0;
}

}  // namespace svg_mb_control::analyze
