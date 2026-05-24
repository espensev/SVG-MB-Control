#include "analyze_report.h"

#include "analyze_db.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace svg_mb_control::analyze {

namespace {

constexpr double kReversalDeadbandPct = 0.35;
constexpr double kSyntheticTickSeconds = 0.05;

enum class Band { kIdle, kLoad, kCooldown };

struct TickRow {
    std::int64_t tick = 0;
    std::string wall_clock;
    double elapsed_s = 0.0;
    Band band = Band::kIdle;
    std::optional<double> cpu_tctl_c;
    std::optional<double> gpu_memjn_c;
    std::optional<double> gpu_envelope_c;
};

struct ChannelStats {
    std::vector<double> setpoint_pct;
    std::vector<double> duty_pct;
    std::vector<double> rpm;
    double max_thermal_pressure_boost_pct = 0.0;
    double max_midband_pressure_boost_pct = 0.0;
    double max_gpu_airflow_boost_pct = 0.0;
    double max_cpu_low_soak_boost_pct = 0.0;
    std::optional<std::int64_t> total_writes_min;
    std::optional<std::int64_t> total_writes_max;
    int reversals = 0;
    int mode_leave_ticks = 0;
    std::optional<double> last_setpoint;
    int last_direction = 0;  // -1, 0, +1
};

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

// Nearest-rank percentile on values sorted ascending: pX is the value at
// index round((X/100) * (n - 1)). p100 is the maximum. Returns nullopt for
// an empty sample.
std::optional<double> Percentile(std::vector<double> values, double pct) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    const double scaled =
        (pct / 100.0) * static_cast<double>(values.size() - 1);
    auto index = static_cast<std::size_t>(std::llround(scaled));
    if (index >= values.size()) {
        index = values.size() - 1;
    }
    return values[index];
}

std::optional<double> Median(std::vector<double> values) {
    return Percentile(std::move(values), 50.0);
}

struct BandPercentiles {
    int n = 0;
    std::optional<double> cpu_tctl_p50, cpu_tctl_p90, cpu_tctl_max;
    std::optional<double> gpu_memjn_p50, gpu_memjn_p90, gpu_memjn_max;
    std::optional<double> gpu_env_p50, gpu_env_p90, gpu_env_max;
};

BandPercentiles SummariseBand(const std::vector<TickRow>& ticks, Band band) {
    std::vector<double> cpu, mem, env;
    for (const auto& t : ticks) {
        if (t.band != band) {
            continue;
        }
        if (t.cpu_tctl_c) cpu.push_back(*t.cpu_tctl_c);
        if (t.gpu_memjn_c) mem.push_back(*t.gpu_memjn_c);
        if (t.gpu_envelope_c) env.push_back(*t.gpu_envelope_c);
    }
    BandPercentiles bp;
    for (const auto& t : ticks) {
        if (t.band == band) ++bp.n;
    }
    bp.cpu_tctl_p50 = Percentile(cpu, 50.0);
    bp.cpu_tctl_p90 = Percentile(cpu, 90.0);
    bp.cpu_tctl_max = Percentile(cpu, 100.0);
    bp.gpu_memjn_p50 = Percentile(mem, 50.0);
    bp.gpu_memjn_p90 = Percentile(mem, 90.0);
    bp.gpu_memjn_max = Percentile(mem, 100.0);
    bp.gpu_env_p50 = Percentile(env, 50.0);
    bp.gpu_env_p90 = Percentile(env, 90.0);
    bp.gpu_env_max = Percentile(env, 100.0);
    return bp;
}

nlohmann::json OptToJson(const std::optional<double>& v) {
    if (!v) {
        return nullptr;
    }
    return *v;
}

std::string OptToText(const std::optional<double>& v) {
    if (!v) {
        return "n/a";
    }
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << *v;
    return os.str();
}

// Aggregated state assembled before output. Both emitters consume this; the
// main function populates it from the SQLite queries.
struct ReportData {
    std::int64_t run_id = 0;
    std::string session_start;
    std::string mode;
    std::string status;
    std::vector<TickRow> ticks;
    BandPercentiles idle;
    BandPercentiles load;
    BandPercentiles cool;
    std::map<int, ChannelStats> channels;
    std::optional<std::int64_t> onset_tick;
    std::optional<std::int64_t> response_tick;
    std::optional<double> response_delay_s;
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;
};

struct TargetRun {
    std::int64_t id = 0;
    std::string session_start;
    std::string mode;
    std::string status;
};

// Resolves the target run row. Caller has already opened the DB and verified
// the schema. Returns nullopt and prints a message when no row matches.
std::optional<TargetRun> LookupTargetRun(Database& db,
                                        const ReportOptions& options,
                                        const std::filesystem::path& db_path) {
    std::string sql = "SELECT id, session_start, mode, status FROM runs ";
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
    return run;
}

void EmitJsonReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out) {
    nlohmann::json doc;
    doc["run"] = {
        {"id", data.run_id},
        {"session_start", data.session_start},
        {"mode", data.mode},
        {"status", data.status},
        {"ticks", static_cast<std::int64_t>(data.ticks.size())},
        {"elapsed_s", data.ticks.back().elapsed_s},
    };
    doc["params"] = {
        {"idle_seconds", options.idle_seconds},
        {"load_threshold_c", options.load_threshold_c},
        {"percentile_method",
         "nearest-rank on sorted ascending, p100=max"},
    };
    auto band_json = [](const BandPercentiles& b) {
        return nlohmann::json{
            {"n", b.n},
            {"cpu_tctl_c",
             {{"p50", OptToJson(b.cpu_tctl_p50)},
              {"p90", OptToJson(b.cpu_tctl_p90)},
              {"max", OptToJson(b.cpu_tctl_max)}}},
            {"gpu_memjn_c",
             {{"p50", OptToJson(b.gpu_memjn_p50)},
              {"p90", OptToJson(b.gpu_memjn_p90)},
              {"max", OptToJson(b.gpu_memjn_max)}}},
            {"gpu_envelope_c",
             {{"p50", OptToJson(b.gpu_env_p50)},
              {"p90", OptToJson(b.gpu_env_p90)},
              {"max", OptToJson(b.gpu_env_max)}}},
        };
    };
    doc["bands"] = {
        {"idle", band_json(data.idle)},
        {"load", band_json(data.load)},
        {"cooldown", band_json(data.cool)},
    };
    doc["channels"] = nlohmann::json::array();
    for (const auto& [ch, cs] : data.channels) {
        std::int64_t writes = 0;
        if (cs.total_writes_min && cs.total_writes_max) {
            writes = *cs.total_writes_max - *cs.total_writes_min;
        }
        doc["channels"].push_back({
            {"channel", ch},
            {"setpoint_pct",
             {{"p50", OptToJson(Percentile(cs.setpoint_pct, 50.0))},
              {"p90", OptToJson(Percentile(cs.setpoint_pct, 90.0))},
              {"max", OptToJson(Percentile(cs.setpoint_pct, 100.0))}}},
            {"duty_pct",
             {{"p50", OptToJson(Percentile(cs.duty_pct, 50.0))},
              {"p90", OptToJson(Percentile(cs.duty_pct, 90.0))},
              {"max", OptToJson(Percentile(cs.duty_pct, 100.0))}}},
            {"rpm",
             {{"p50", OptToJson(Percentile(cs.rpm, 50.0))},
              {"p90", OptToJson(Percentile(cs.rpm, 90.0))},
              {"max", OptToJson(Percentile(cs.rpm, 100.0))}}},
            {"max_thermal_pressure_boost_pct",
             cs.max_thermal_pressure_boost_pct},
            {"max_midband_pressure_boost_pct",
             cs.max_midband_pressure_boost_pct},
            {"max_gpu_airflow_boost_pct",
             cs.max_gpu_airflow_boost_pct},
            {"max_cpu_low_soak_boost_pct",
             cs.max_cpu_low_soak_boost_pct},
            {"writes", writes},
            {"reversals", cs.reversals},
            {"mode_leave_ticks", cs.mode_leave_ticks},
        });
    }
    doc["response"] = {
        {"load_onset_tick",
         data.onset_tick ? nlohmann::json(*data.onset_tick)
                         : nlohmann::json()},
        {"first_setpoint_increase_tick",
         data.response_tick ? nlohmann::json(*data.response_tick)
                            : nlohmann::json()},
        {"response_delay_s", OptToJson(data.response_delay_s)},
    };
    doc["robustness"] = {
        {"authority_reasserted", data.authority_reasserted},
        {"write_failures", data.write_failures},
        {"restore_failures", data.restore_failures},
    };
    out << doc.dump(2) << '\n';
}

void EmitTextReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out) {
    std::ostringstream os;
    os << "analyze report: run_id=" << data.run_id
       << " session_start=" << data.session_start << " mode=" << data.mode
       << " status=" << data.status << '\n';
    os << "  ticks=" << data.ticks.size()
       << " elapsed_s=" << data.ticks.back().elapsed_s
       << " idle_seconds=" << options.idle_seconds
       << " load_threshold_c=" << options.load_threshold_c << '\n';
    os << "  band sample counts: idle=" << data.idle.n
       << " load=" << data.load.n << " cooldown=" << data.cool.n << '\n';
    os << "  percentile method: nearest-rank on sorted ascending, p100=max\n";
    auto print_band = [&os](const char* name, const BandPercentiles& b) {
        os << "[" << name << "] cpu_tctl_c p50=" << OptToText(b.cpu_tctl_p50)
           << " p90=" << OptToText(b.cpu_tctl_p90)
           << " max=" << OptToText(b.cpu_tctl_max)
           << "  gpu_memjn_c p50=" << OptToText(b.gpu_memjn_p50)
           << " p90=" << OptToText(b.gpu_memjn_p90)
           << " max=" << OptToText(b.gpu_memjn_max)
           << "  gpu_envelope_c p50=" << OptToText(b.gpu_env_p50)
           << " p90=" << OptToText(b.gpu_env_p90)
           << " max=" << OptToText(b.gpu_env_max) << '\n';
    };
    print_band("idle", data.idle);
    print_band("load", data.load);
    print_band("cooldown", data.cool);
    os << "channels:\n";
    for (const auto& [ch, cs] : data.channels) {
        std::int64_t writes = 0;
        if (cs.total_writes_min && cs.total_writes_max) {
            writes = *cs.total_writes_max - *cs.total_writes_min;
        }
        os << "  ch" << ch << " setpoint_pct p50="
           << OptToText(Percentile(cs.setpoint_pct, 50.0))
           << " p90=" << OptToText(Percentile(cs.setpoint_pct, 90.0))
           << " max=" << OptToText(Percentile(cs.setpoint_pct, 100.0))
           << "  duty_pct p50=" << OptToText(Percentile(cs.duty_pct, 50.0))
           << " p90=" << OptToText(Percentile(cs.duty_pct, 90.0))
           << " max=" << OptToText(Percentile(cs.duty_pct, 100.0))
           << "  rpm p50=" << OptToText(Percentile(cs.rpm, 50.0))
           << " p90=" << OptToText(Percentile(cs.rpm, 90.0))
           << " max=" << OptToText(Percentile(cs.rpm, 100.0)) << '\n';
        os << "       thermal_pressure_boost_pct max="
           << cs.max_thermal_pressure_boost_pct
           << "  midband_pressure_boost_pct max="
           << cs.max_midband_pressure_boost_pct
           << "  gpu_airflow_boost_pct max="
           << cs.max_gpu_airflow_boost_pct
           << "  cpu_low_soak_boost_pct max="
           << cs.max_cpu_low_soak_boost_pct << "  writes=" << writes
           << "  reversals=" << cs.reversals
           << "  mode_leave_ticks=" << cs.mode_leave_ticks << '\n';
    }
    os << "response: load_onset_tick="
       << (data.onset_tick ? std::to_string(*data.onset_tick) : "n/a")
       << " first_setpoint_increase_tick="
       << (data.response_tick ? std::to_string(*data.response_tick) : "n/a")
       << " response_delay_s=" << OptToText(data.response_delay_s) << '\n';
    os << "robustness: authority_reasserted=" << data.authority_reasserted
       << " write_failures=" << data.write_failures
       << " restore_failures=" << data.restore_failures << '\n';
    out << os.str();
}

}  // namespace

int RunAnalyzeReport(const ReportOptions& options) {
    const std::filesystem::path db_path = options.db_path.empty()
        ? options.runtime_home / "svg_mb_control.db"
        : options.db_path;

    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec) || ec) {
        std::cerr << "Error: database does not exist: " << db_path.string()
                  << '\n';
        return 1;
    }

    Database db;
    try {
        db.Open(db_path);
        if (GetSchemaVersion(db) <= 0) {
            std::cerr << "Error: database schema is missing.\n";
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: cannot open database: " << ex.what() << '\n';
        return 1;
    }

    std::optional<TargetRun> target;
    try {
        target = LookupTargetRun(db, options, db_path);
    } catch (const std::exception& ex) {
        std::cerr << "Error: run lookup failed: " << ex.what() << '\n';
        return 1;
    }
    if (!target.has_value()) {
        return 1;
    }
    const std::int64_t run_id = target->id;

    std::vector<TickRow> ticks;
    std::map<int, ChannelStats> channels;
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;

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
            ticks.push_back(std::move(row));
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: tick query failed: " << ex.what() << '\n';
        return 1;
    }

    if (ticks.empty()) {
        std::cerr << "Error: run " << run_id << " has no ingested ticks.\n";
        return 1;
    }

    // Elapsed time. Prefer parsed wall_clock; if it never advances across the
    // run, synthesize 50 ms per tick so the bands still segment.
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
            ticks[i].elapsed_s = static_cast<double>(i) * kSyntheticTickSeconds;
        }
    }

    // Band assignment. Idle: elapsed < idle_seconds. Cooldown: the trailing
    // run of ticks after the last tick that reached the load threshold, as
    // long as that hot tick is past the idle band. Load: everything else.
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

    // Per-channel control samples.
    try {
        Statement stmt = db.Prepare(
            "SELECT tick_count, channel, setpoint_pct, "
            "thermal_pressure_boost_pct, midband_pressure_boost_pct, "
            "gpu_airflow_boost_pct, cpu_low_soak_boost_pct, total_writes "
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
            auto writes = ColumnOptionalInt(stmt, 7);
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
        return 1;
    }

    // Per-channel fan samples (fan_index maps to the controlled channel
    // index on this board: channels 0-5).
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
        return 1;
    }

    // Robustness events.
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
                authority_reasserted = static_cast<int>(stmt.ColumnInt(0));
            }
            if (!stmt.ColumnIsNull(1)) {
                write_failures = static_cast<int>(stmt.ColumnInt(1));
            }
            if (!stmt.ColumnIsNull(2)) {
                restore_failures = static_cast<int>(stmt.ColumnInt(2));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: event query failed: " << ex.what() << '\n';
        return 1;
    }

    // Idle-band setpoint baseline per channel for response detection.
    std::map<int, double> idle_setpoint_baseline;
    {
        std::map<int, std::vector<double>> idle_setpoints;
        Statement stmt = db.Prepare(
            "SELECT tick_count, channel, setpoint_pct FROM "
            "tick_channel_samples WHERE run_id = ?1 ORDER BY tick_count ASC");
        stmt.BindInt(1, run_id);
        // Map tick_count -> band by lookup.
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
        for (auto& [ch, vals] : idle_setpoints) {
            if (auto m = Median(vals)) {
                idle_setpoint_baseline[ch] = *m;
            }
        }
    }

    // Response delay: time from the first load-threshold crossing to the
    // first controlled-channel setpoint increase above its idle baseline.
    std::optional<std::int64_t> onset_tick;
    std::optional<double> onset_elapsed;
    for (const auto& t : ticks) {
        const bool hot =
            (t.cpu_tctl_c && *t.cpu_tctl_c >= options.load_threshold_c) ||
            (t.gpu_envelope_c &&
             *t.gpu_envelope_c >= options.load_threshold_c);
        if (hot) {
            onset_tick = t.tick;
            onset_elapsed = t.elapsed_s;
            break;
        }
    }
    std::optional<std::int64_t> response_tick;
    std::optional<double> response_delay_s;
    if (onset_tick) {
        std::map<std::int64_t, double> tick_elapsed;
        for (const auto& t : ticks) {
            tick_elapsed[t.tick] = t.elapsed_s;
        }
        Statement stmt = db.Prepare(
            "SELECT tick_count, channel, setpoint_pct FROM "
            "tick_channel_samples WHERE run_id = ?1 AND tick_count >= ?2 "
            "ORDER BY tick_count ASC");
        stmt.BindInt(1, run_id);
        stmt.BindInt(2, *onset_tick);
        while (stmt.Step()) {
            if (stmt.ColumnIsNull(2)) {
                continue;
            }
            const std::int64_t tick = stmt.ColumnInt(0);
            const int channel = static_cast<int>(stmt.ColumnInt(1));
            const double setpoint = stmt.ColumnDouble(2);
            auto base_it = idle_setpoint_baseline.find(channel);
            const double base = base_it != idle_setpoint_baseline.end()
                ? base_it->second
                : setpoint;
            if (setpoint > base + kReversalDeadbandPct) {
                response_tick = tick;
                auto te = tick_elapsed.find(tick);
                if (te != tick_elapsed.end() && onset_elapsed) {
                    response_delay_s = te->second - *onset_elapsed;
                }
                break;
            }
        }
    }

    ReportData data;
    data.run_id = run_id;
    data.session_start = target->session_start;
    data.mode = target->mode;
    data.status = target->status;
    data.ticks = std::move(ticks);
    data.idle = SummariseBand(data.ticks, Band::kIdle);
    data.load = SummariseBand(data.ticks, Band::kLoad);
    data.cool = SummariseBand(data.ticks, Band::kCooldown);
    data.channels = std::move(channels);
    data.onset_tick = onset_tick;
    data.response_tick = response_tick;
    data.response_delay_s = response_delay_s;
    data.authority_reasserted = authority_reasserted;
    data.write_failures = write_failures;
    data.restore_failures = restore_failures;

    if (options.as_json) {
        EmitJsonReport(options, data, std::cout);
    } else {
        EmitTextReport(options, data, std::cout);
    }
    return 0;
}

}  // namespace svg_mb_control::analyze
