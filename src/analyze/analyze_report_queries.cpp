#include "analyze_report_queries.h"

#include "analyze_db.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace svg_mb_control::analyze::report_detail {

namespace {

constexpr double kReversalDeadbandPct = 0.35;
constexpr double kSyntheticTickSeconds = 0.05;

std::optional<double> ColumnOptionalDouble(Statement& stmt, int idx) {
    if (stmt.ColumnIsNull(idx)) {
        return std::nullopt;
    }
    return stmt.ColumnDouble(idx);
}

std::optional<std::int64_t> ColumnOptionalInt(Statement& stmt, int idx) {
    if (stmt.ColumnIsNull(idx)) {
        return std::nullopt;
    }
    return stmt.ColumnInt(idx);
}

std::optional<std::string> ColumnOptionalText(Statement& stmt, int idx) {
    if (stmt.ColumnIsNull(idx)) {
        return std::nullopt;
    }
    return stmt.ColumnText(idx);
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Valid for the date ranges this tool sees.
std::int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Parses wall_clock of the form YYYY-MM-DDThh:mm:ss[.fff][zone]. Returns
// seconds (timezone offsets are ignored; all rows in a run share one zone).
std::optional<double> ParseWallClockSeconds(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double s = 0.0;
    char sep1 = 0, sep2 = 0, sep3 = 0, tsep = 0;
    std::istringstream in(text);
    in >> y >> sep1 >> mo >> sep2 >> d >> tsep >> h >> sep3 >> mi;
    if (in.fail() || sep1 != '-' || sep2 != '-' || sep3 != ':' ||
        (tsep != 'T' && tsep != ' ')) {
        return std::nullopt;
    }
    char colon = 0;
    if (!(in >> colon) || colon != ':') {
        return std::nullopt;
    }
    if (!(in >> s)) {
        return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return std::nullopt;
    }
    const std::int64_t days =
        DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return static_cast<double>(days) * 86400.0 +
           static_cast<double>(h) * 3600.0 + static_cast<double>(mi) * 60.0 + s;
}

std::string JsonStringMember(const nlohmann::json& node, const char* key) {
    if (!node.is_object()) {
        return {};
    }
    const auto it = node.find(key);
    return it != node.end() && it->is_string() ? it->get<std::string>()
                                               : std::string();
}

void ReadManifestIdentity(const nlohmann::json& root,
                          const char* key,
                          std::filesystem::path& path_out,
                          std::string& sha256_out) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        return;
    }
    const std::string path = JsonStringMember(*it, "path");
    if (!path.empty()) {
        path_out = path;
    }
    sha256_out = JsonStringMember(*it, "sha256");
}

}  // namespace

// Resolves the target run row. Caller has already opened the DB and verified
// the schema. Returns nullopt and prints a message when no row matches.
std::optional<TargetRun> LookupTargetRun(
    Database& db,
    const ReportOptions& options,
    const std::filesystem::path& db_path) {
    std::string sql =
        "SELECT id, session_start, mode, status, manifest_path, "
        "csv_archive_path, tool_version, git_hash, row_count_declared, "
        "row_count_ingested, event_count_declared, event_count_ingested, "
        "csv_flush_policy, mirror_mode, last_update FROM runs ";
    if (options.run_id) {
        sql += "WHERE id = ?1";
    } else if (options.session_start) {
        sql += "WHERE session_start = ?1 ORDER BY id DESC";
    } else {
        sql += "ORDER BY session_start DESC, id DESC";
    }
    sql += " LIMIT 1";
    Statement stmt = db.Prepare(sql);
    if (options.run_id) {
        stmt.BindInt(1, *options.run_id);
    } else if (options.session_start) {
        stmt.BindText(1, *options.session_start);
    }
    if (!stmt.Step()) {
        std::cerr << "Error: no matching run in " << db_path.string() << '\n';
        return std::nullopt;
    }
    TargetRun run;
    run.id = stmt.ColumnInt(0);
    run.session_start = stmt.ColumnText(1);
    run.mode = stmt.ColumnText(2);
    run.status = stmt.ColumnText(3);
    run.manifest_path = stmt.ColumnText(4);
    run.csv_archive_path = stmt.ColumnText(5);
    run.tool_version = ColumnOptionalText(stmt, 6);
    run.git_hash = ColumnOptionalText(stmt, 7);
    run.row_count_declared = ColumnOptionalInt(stmt, 8);
    run.row_count_ingested = ColumnOptionalInt(stmt, 9);
    run.event_count_declared = ColumnOptionalInt(stmt, 10);
    run.event_count_ingested = ColumnOptionalInt(stmt, 11);
    run.csv_flush_policy = ColumnOptionalText(stmt, 12);
    run.mirror_mode = ColumnOptionalText(stmt, 13);
    run.last_update = ColumnOptionalText(stmt, 14);
    return run;
}

RuntimeManifestEvidence LoadRuntimeManifestEvidence(
    const std::filesystem::path& manifest_path) {
    RuntimeManifestEvidence evidence;
    if (manifest_path.empty()) {
        return evidence;
    }
    std::ifstream stream(manifest_path);
    if (!stream) {
        return evidence;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error&) {
        return evidence;
    }
    if (!root.is_object()) {
        return evidence;
    }

    ReadManifestIdentity(root, "config", evidence.config_path,
                         evidence.config_sha256);
    ReadManifestIdentity(root, "runtime_policy",
                         evidence.runtime_policy_path,
                         evidence.runtime_policy_sha256);

    const auto artifacts = root.find("artifacts");
    if (artifacts != root.end() && artifacts->is_object()) {
        const auto events = artifacts->find("events");
        if (events != artifacts->end() && events->is_object()) {
            const std::string path = JsonStringMember(*events, "path");
            if (!path.empty()) {
                evidence.events_path = path;
            }
        }
    }
    return evidence;
}

// Loads tick_samples for run_id into `out`, sorted ascending by tick_count.
// Returns false on SQL failure (logs the same "tick query failed" prefix the
// orchestrator used to emit inline).
bool LoadTicks(Database& db, std::int64_t run_id,
               std::vector<TickRow>& out) {
    try {
        Statement stmt = db.Prepare(
            "SELECT tick_count, wall_clock, cpu_tctl_c, gpu_memjn_c, "
            "gpu_envelope_c FROM tick_samples WHERE run_id = ?1 "
            "ORDER BY tick_count ASC");
        stmt.BindInt(1, run_id);
        while (stmt.Step()) {
            TickRow row;
            row.tick = stmt.ColumnInt(0);
            row.wall_clock = stmt.ColumnText(1);
            row.cpu_tctl_c = ColumnOptionalDouble(stmt, 2);
            row.gpu_memjn_c = ColumnOptionalDouble(stmt, 3);
            row.gpu_envelope_c = ColumnOptionalDouble(stmt, 4);
            out.push_back(std::move(row));
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: tick query failed: " << ex.what() << '\n';
        return false;
    }
    return true;
}

// Sets elapsed_s on each tick using parsed wall_clock when it advances; falls
// back to a synthetic 50 ms/tick stride when the timestamp never moves, so
// band assignment still has a usable time axis.
void ComputeElapsedTime(std::vector<TickRow>& ticks) {
    std::optional<double> first_parsed;
    bool any_advance = false;
    {
        std::optional<double> prev;
        for (const auto& t : ticks) {
            auto parsed = ParseWallClockSeconds(t.wall_clock);
            if (parsed && !first_parsed) {
                first_parsed = parsed;
            }
            if (parsed && prev && *parsed > *prev) {
                any_advance = true;
            }
            if (parsed) {
                prev = parsed;
            }
        }
    }
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        auto parsed = ParseWallClockSeconds(ticks[i].wall_clock);
        if (first_parsed && any_advance && parsed) {
            ticks[i].elapsed_s = *parsed - *first_parsed;
        } else {
            ticks[i].elapsed_s =
                static_cast<double>(i) * kSyntheticTickSeconds;
        }
    }
}

// Assigns each tick a band:
//   - kIdle:     elapsed_s < idle_seconds
//   - kCooldown: trailing run after the last hot tick (only when that hot
//                tick is past the idle band)
//   - kLoad:     everything else
void AssignBands(std::vector<TickRow>& ticks, const ReportOptions& options) {
    const double idle_cut = static_cast<double>(options.idle_seconds);
    std::optional<std::size_t> last_hot;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const auto& t = ticks[i];
        const bool hot =
            (t.cpu_tctl_c && *t.cpu_tctl_c >= options.load_threshold_c) ||
            (t.gpu_envelope_c &&
             *t.gpu_envelope_c >= options.load_threshold_c);
        if (hot && t.elapsed_s >= idle_cut) {
            last_hot = i;
        }
    }
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        if (ticks[i].elapsed_s < idle_cut) {
            ticks[i].band = Band::kIdle;
        } else if (last_hot && i > *last_hot) {
            ticks[i].band = Band::kCooldown;
        } else {
            ticks[i].band = Band::kLoad;
        }
    }
}

// Loads tick_channel_samples for run_id and aggregates per-channel stats:
// setpoint/boost maxima, reversal count via the kReversalDeadbandPct gate,
// primary-source counts, and total_writes range. Channels are created
// lazily on first sample.
bool LoadChannelStats(Database& db, std::int64_t run_id,
                      std::map<int, ChannelStats>& channels) {
    try {
        Statement stmt = db.Prepare(
            "SELECT tick_count, channel, setpoint_pct, "
            "thermal_pressure_boost_pct, midband_pressure_boost_pct, "
            "gpu_airflow_boost_pct, cpu_low_soak_boost_pct, "
            "primary_temp_source, response_source, write_reason, "
            "total_writes "
            "FROM tick_channel_samples WHERE run_id = ?1 "
            "ORDER BY channel ASC, tick_count ASC");
        stmt.BindInt(1, run_id);
        while (stmt.Step()) {
            const int channel = static_cast<int>(stmt.ColumnInt(1));
            ChannelStats& cs = channels[channel];
            auto setpoint = ColumnOptionalDouble(stmt, 2);
            auto tp = ColumnOptionalDouble(stmt, 3);
            auto mid = ColumnOptionalDouble(stmt, 4);
            auto gpu = ColumnOptionalDouble(stmt, 5);
            auto soak = ColumnOptionalDouble(stmt, 6);
            std::string primary_source = "unavailable";
            if (!stmt.ColumnIsNull(7)) {
                primary_source = stmt.ColumnText(7);
                if (primary_source.empty()) {
                    primary_source = "unavailable";
                }
            }
            ++cs.primary_source_counts[primary_source];
            std::string response_source = "unavailable";
            if (!stmt.ColumnIsNull(8)) {
                response_source = stmt.ColumnText(8);
                if (response_source.empty()) {
                    response_source = "unavailable";
                }
            }
            ++cs.response_source_counts[response_source];
            std::string write_reason = "unavailable";
            if (!stmt.ColumnIsNull(9)) {
                write_reason = stmt.ColumnText(9);
                if (write_reason.empty()) {
                    write_reason = "unavailable";
                }
            }
            ++cs.write_reason_counts[write_reason];
            auto writes = ColumnOptionalInt(stmt, 10);
            if (setpoint) {
                cs.setpoint_pct.push_back(*setpoint);
                if (cs.last_setpoint) {
                    const double delta = *setpoint - *cs.last_setpoint;
                    if (std::abs(delta) > kReversalDeadbandPct) {
                        const int dir = delta > 0 ? 1 : -1;
                        if (cs.last_direction != 0 &&
                            dir != cs.last_direction) {
                            ++cs.reversals;
                        }
                        cs.last_direction = dir;
                    }
                }
                cs.last_setpoint = *setpoint;
            }
            if (tp) {
                cs.max_thermal_pressure_boost_pct =
                    std::max(cs.max_thermal_pressure_boost_pct, *tp);
            }
            if (mid) {
                cs.max_midband_pressure_boost_pct =
                    std::max(cs.max_midband_pressure_boost_pct, *mid);
            }
            if (gpu) {
                cs.max_gpu_airflow_boost_pct =
                    std::max(cs.max_gpu_airflow_boost_pct, *gpu);
            }
            if (soak) {
                cs.max_cpu_low_soak_boost_pct =
                    std::max(cs.max_cpu_low_soak_boost_pct, *soak);
            }
            if (writes) {
                if (!cs.total_writes_min || *writes < *cs.total_writes_min) {
                    cs.total_writes_min = *writes;
                }
                if (!cs.total_writes_max || *writes > *cs.total_writes_max) {
                    cs.total_writes_max = *writes;
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: channel query failed: " << ex.what() << '\n';
        return false;
    }
    return true;
}

// Merges fan-side samples (duty/rpm/mode_raw) into existing channel stats.
// fan_index maps directly to the controlled channel index on this board
// (channels 0-5); fans with no matching channel from LoadChannelStats are
// skipped.
bool MergeFanStats(Database& db, std::int64_t run_id,
                   std::map<int, ChannelStats>& channels) {
    try {
        Statement stmt = db.Prepare(
            "SELECT fan_index, duty_pct, rpm, mode_raw FROM tick_fan_samples "
            "WHERE run_id = ?1");
        stmt.BindInt(1, run_id);
        while (stmt.Step()) {
            const int fan = static_cast<int>(stmt.ColumnInt(0));
            auto it = channels.find(fan);
            if (it == channels.end()) {
                continue;
            }
            ChannelStats& cs = it->second;
            if (auto duty = ColumnOptionalDouble(stmt, 1)) {
                cs.duty_pct.push_back(*duty);
            }
            if (auto rpm = ColumnOptionalDouble(stmt, 2)) {
                cs.rpm.push_back(*rpm);
            }
            if (auto mode_raw = ColumnOptionalInt(stmt, 3)) {
                if (*mode_raw != 0) {
                    ++cs.mode_leave_ticks;
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: fan query failed: " << ex.what() << '\n';
        return false;
    }
    return true;
}

// Counts robustness events for run_id: authority reasserts, write failures,
// and restore failures via the existing event_type LIKE patterns.
bool LoadRobustnessCounts(Database& db, std::int64_t run_id,
                          RobustnessCounts& out) {
    try {
        Statement stmt = db.Prepare(
            "SELECT "
            "SUM(event_type LIKE '%authority%'), "
            "SUM(event_type LIKE 'control_loop.write%' AND success = 0), "
            "SUM(event_type LIKE '%restore%' AND success = 0) "
            "FROM events WHERE run_id = ?1");
        stmt.BindInt(1, run_id);
        if (stmt.Step()) {
            if (!stmt.ColumnIsNull(0)) {
                out.authority_reasserted =
                    static_cast<int>(stmt.ColumnInt(0));
            }
            if (!stmt.ColumnIsNull(1)) {
                out.write_failures = static_cast<int>(stmt.ColumnInt(1));
            }
            if (!stmt.ColumnIsNull(2)) {
                out.restore_failures = static_cast<int>(stmt.ColumnInt(2));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: event query failed: " << ex.what() << '\n';
        return false;
    }
    return true;
}

bool LoadEventFieldCounts(Database& db,
                          std::int64_t run_id,
                          const char* field_name,
                          const char* fallback,
                          std::map<std::string, int>& out) {
    try {
        std::string sql =
            "SELECT COALESCE(NULLIF(" + std::string(field_name) +
            ", ''), ?2), COUNT(*) FROM events WHERE run_id = ?1 "
            "GROUP BY COALESCE(NULLIF(" +
            std::string(field_name) + ", ''), ?2) ORDER BY 1";
        Statement stmt = db.Prepare(sql);
        stmt.BindInt(1, run_id);
        stmt.BindText(2, fallback);
        while (stmt.Step()) {
            out[stmt.ColumnText(0)] = static_cast<int>(stmt.ColumnInt(1));
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: event count query failed: " << ex.what()
                  << '\n';
        return false;
    }
    return true;
}

// Per-channel median setpoint while in the idle band, used as the baseline
// for response detection.
std::map<int, double> ComputeIdleSetpointBaselines(
    Database& db, std::int64_t run_id,
    const std::vector<TickRow>& ticks) {
    std::map<int, std::vector<double>> idle_setpoints;
    Statement stmt = db.Prepare(
        "SELECT tick_count, channel, setpoint_pct FROM "
        "tick_channel_samples WHERE run_id = ?1 ORDER BY tick_count ASC");
    stmt.BindInt(1, run_id);
    std::map<std::int64_t, Band> tick_band;
    for (const auto& t : ticks) {
        tick_band[t.tick] = t.band;
    }
    while (stmt.Step()) {
        const std::int64_t tick = stmt.ColumnInt(0);
        const int channel = static_cast<int>(stmt.ColumnInt(1));
        if (stmt.ColumnIsNull(2)) {
            continue;
        }
        auto band_it = tick_band.find(tick);
        if (band_it != tick_band.end() &&
            band_it->second == Band::kIdle) {
            idle_setpoints[channel].push_back(stmt.ColumnDouble(2));
        }
    }
    std::map<int, double> out;
    for (auto& [ch, vals] : idle_setpoints) {
        if (auto m = Median(vals)) {
            out[ch] = *m;
        }
    }
    return out;
}

// Wall-clock delay between the first hot tick and the first per-channel
// setpoint increase above the channel's idle baseline. When no tick crosses
// the load threshold the onset is nullopt; when no channel responds within
// the run, response_tick / response_delay_s stay nullopt.
ResponseDelay DetectResponseDelay(
    Database& db, std::int64_t run_id,
    const std::vector<TickRow>& ticks,
    const std::map<int, double>& idle_baselines,
    const ReportOptions& options) {
    ResponseDelay result;
    std::optional<double> onset_elapsed;
    for (const auto& t : ticks) {
        const bool hot =
            (t.cpu_tctl_c && *t.cpu_tctl_c >= options.load_threshold_c) ||
            (t.gpu_envelope_c &&
             *t.gpu_envelope_c >= options.load_threshold_c);
        if (hot) {
            result.onset_tick = t.tick;
            onset_elapsed = t.elapsed_s;
            break;
        }
    }
    if (!result.onset_tick) {
        return result;
    }

    std::map<std::int64_t, double> tick_elapsed;
    for (const auto& t : ticks) {
        tick_elapsed[t.tick] = t.elapsed_s;
    }
    Statement stmt = db.Prepare(
        "SELECT tick_count, channel, setpoint_pct FROM "
        "tick_channel_samples WHERE run_id = ?1 AND tick_count >= ?2 "
        "ORDER BY tick_count ASC");
    stmt.BindInt(1, run_id);
    stmt.BindInt(2, *result.onset_tick);
    while (stmt.Step()) {
        if (stmt.ColumnIsNull(2)) {
            continue;
        }
        const std::int64_t tick = stmt.ColumnInt(0);
        const int channel = static_cast<int>(stmt.ColumnInt(1));
        const double setpoint = stmt.ColumnDouble(2);
        auto base_it = idle_baselines.find(channel);
        const double base = base_it != idle_baselines.end()
            ? base_it->second
            : setpoint;
        if (setpoint > base + kReversalDeadbandPct) {
            result.response_tick = tick;
            auto te = tick_elapsed.find(tick);
            if (te != tick_elapsed.end() && onset_elapsed) {
                result.response_delay_s = te->second - *onset_elapsed;
            }
            break;
        }
    }
    return result;
}

}  // namespace svg_mb_control::analyze::report_detail
