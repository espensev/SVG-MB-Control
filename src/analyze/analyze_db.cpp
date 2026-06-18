#include "analyze_db.h"

#include "analyze_channel_sample_columns.h"
#include "sqlite3.h"

#include <utility>

namespace svg_mb_control::analyze {

namespace {

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    manifest_path TEXT NOT NULL UNIQUE,
    csv_archive_path TEXT,
    session_start TEXT NOT NULL,
    mode TEXT NOT NULL,
    status TEXT NOT NULL,
    tool_version TEXT,
    git_hash TEXT,
    row_count_declared INTEGER,
    row_count_ingested INTEGER NOT NULL DEFAULT 0,
    event_count_declared INTEGER,
    event_count_ingested INTEGER NOT NULL DEFAULT 0,
    csv_flush_policy TEXT,
    mirror_mode TEXT,
    last_update TEXT,
    ingested_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_runs_session_start ON runs(session_start);
CREATE INDEX IF NOT EXISTS idx_runs_mode ON runs(mode);

CREATE TABLE IF NOT EXISTS tick_samples (
    run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    tick_count INTEGER NOT NULL,
    wall_clock TEXT NOT NULL,
    mode TEXT,
    snapshot_time TEXT,
    snapshot_age_ms INTEGER,
    amd_sensor_count INTEGER,
    amd_sensor_summary TEXT,
    cpu_tctl_c REAL,
    cpu_max_c REAL,
    gpu_available INTEGER,
    gpu_name TEXT,
    gpu_last_warning TEXT,
    gpu_core_c REAL,
    gpu_memjn_c REAL,
    gpu_hotspot_c REAL,
    gpu_envelope_c REAL,
    fan_count INTEGER,
    policy_writes_enabled_present INTEGER,
    policy_writes_enabled INTEGER,
    loop_started_wall_clock TEXT,
    loop_finished_wall_clock TEXT,
    loop_work_duration_ms REAL,
    loop_intended_interval_ms INTEGER,
    loop_achieved_interval_ms REAL,
    loop_slip_ms REAL,
    loop_overrun INTEGER,
    process_cpu_delta_ms REAL,
    process_cpu_pct REAL,
    process_working_set_bytes INTEGER,
    process_private_bytes INTEGER,
    cadence_transient REAL,
    cpu_power_sample_id INTEGER,
    cpu_power_window_ms REAL,
    cpu_pkg_energy_delta_uj REAL,
    cpu_pkg_energy_acquisition TEXT,
    cpu_cycles_sample_id INTEGER,
    cpu_cycles_window_ms REAL,
    cpu_aperf_delta REAL,
    cpu_mperf_delta REAL,
    cpu_cycles_acquisition TEXT,
    gpu_power_sample_id INTEGER,
    gpu_power_time_ms REAL,
    gpu_power_mw REAL,
    gpu_power_source TEXT,
    gpu_power_acquisition TEXT,
    PRIMARY KEY (run_id, tick_count)
);

CREATE INDEX IF NOT EXISTS idx_tick_samples_wall_clock
    ON tick_samples(run_id, wall_clock);
CREATE INDEX IF NOT EXISTS idx_tick_samples_gpu_envelope
    ON tick_samples(run_id, gpu_envelope_c);

CREATE TABLE IF NOT EXISTS tick_fan_samples (
    run_id INTEGER NOT NULL,
    tick_count INTEGER NOT NULL,
    fan_index INTEGER NOT NULL,
    present INTEGER,
    label TEXT,
    rpm INTEGER,
    tach_raw INTEGER,
    tach_valid INTEGER,
    duty_raw INTEGER,
    duty_pct REAL,
    mode_raw INTEGER,
    manual_override INTEGER,
    write_allowed INTEGER,
    policy_blocked INTEGER,
    effective_write_allowed INTEGER,
    PRIMARY KEY (run_id, tick_count, fan_index),
    FOREIGN KEY (run_id, tick_count)
        REFERENCES tick_samples(run_id, tick_count) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER REFERENCES runs(id) ON DELETE CASCADE,
    event_time TEXT NOT NULL,
    event_type TEXT NOT NULL,
    severity TEXT,
    error_code TEXT,
    mode TEXT,
    success INTEGER,
    channel INTEGER,
    setpoint_pct REAL,
    observed_temp_c REAL,
    tick_count INTEGER,
    detail TEXT,
    extra_json TEXT
);

CREATE INDEX IF NOT EXISTS idx_events_run_time ON events(run_id, event_time);
CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type);
CREATE INDEX IF NOT EXISTS idx_events_severity ON events(severity, error_code);

CREATE TABLE IF NOT EXISTS plant_model_captures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_path TEXT NOT NULL UNIQUE,
    captured_local TEXT NOT NULL,
    abort_reason TEXT,
    settle_window_ms INTEGER NOT NULL,
    abort_temp_ceiling_c REAL NOT NULL,
    tool_version TEXT,
    git_hash TEXT,
    sequence_json TEXT NOT NULL,
    ingested_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS plant_model_channels (
    capture_id INTEGER NOT NULL REFERENCES plant_model_captures(id)
        ON DELETE CASCADE,
    channel INTEGER NOT NULL,
    baseline_captured INTEGER,
    restored INTEGER,
    baseline_duty_raw INTEGER,
    baseline_mode_raw INTEGER,
    baseline_rpm REAL,
    baseline_tctl_c REAL,
    baseline_gpu_envelope_c REAL,
    note TEXT,
    PRIMARY KEY (capture_id, channel)
);

CREATE TABLE IF NOT EXISTS plant_model_steps (
    capture_id INTEGER NOT NULL,
    channel INTEGER NOT NULL,
    step_index INTEGER NOT NULL,
    duty_pct_target REAL NOT NULL,
    hold_ms INTEGER NOT NULL,
    settle_window_ms INTEGER NOT NULL,
    settle_sample_count INTEGER,
    duty_pct_observed_mean REAL,
    rpm_mean REAL,
    rpm_stddev REAL,
    tctl_c_mean REAL,
    gpu_envelope_c_mean REAL,
    PRIMARY KEY (capture_id, channel, step_index),
    FOREIGN KEY (capture_id, channel)
        REFERENCES plant_model_channels(capture_id, channel) ON DELETE CASCADE
);
)sql";

[[noreturn]] void Throw(sqlite3* db, std::string_view context, int code) {
    std::string msg(context);
    if (db != nullptr) {
        const char* err = sqlite3_errmsg(db);
        if (err != nullptr) {
            msg += ": ";
            msg += err;
        }
    }
    throw SqliteError(std::move(msg), code);
}

}  // namespace

SqliteError::SqliteError(std::string message, int code)
    : std::runtime_error(std::move(message)), code_(code) {}

Statement::Statement(sqlite3* db, std::string_view sql) {
    const int rc = sqlite3_prepare_v2(
        db,
        sql.data(),
        static_cast<int>(sql.size()),
        &stmt_,
        nullptr);
    if (rc != SQLITE_OK) {
        Throw(db, "sqlite3_prepare_v2", rc);
    }
}

Statement::~Statement() {
    Finalize();
}

Statement::Statement(Statement&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        Finalize();
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

void Statement::Finalize() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

void Statement::BindNull(int idx) {
    const int rc = sqlite3_bind_null(stmt_, idx);
    if (rc != SQLITE_OK) {
        Throw(sqlite3_db_handle(stmt_), "sqlite3_bind_null", rc);
    }
}

void Statement::BindInt(int idx, std::int64_t value) {
    const int rc = sqlite3_bind_int64(stmt_, idx, value);
    if (rc != SQLITE_OK) {
        Throw(sqlite3_db_handle(stmt_), "sqlite3_bind_int64", rc);
    }
}

void Statement::BindDouble(int idx, double value) {
    const int rc = sqlite3_bind_double(stmt_, idx, value);
    if (rc != SQLITE_OK) {
        Throw(sqlite3_db_handle(stmt_), "sqlite3_bind_double", rc);
    }
}

void Statement::BindText(int idx, std::string_view value) {
    const int rc = sqlite3_bind_text(
        stmt_,
        idx,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        Throw(sqlite3_db_handle(stmt_), "sqlite3_bind_text", rc);
    }
}

void Statement::BindOptionalInt(int idx, std::optional<std::int64_t> value) {
    if (value.has_value()) {
        BindInt(idx, *value);
    } else {
        BindNull(idx);
    }
}

void Statement::BindOptionalDouble(int idx, std::optional<double> value) {
    if (value.has_value()) {
        BindDouble(idx, *value);
    } else {
        BindNull(idx);
    }
}

void Statement::BindOptionalText(int idx,
                                 const std::optional<std::string>& value) {
    if (value.has_value()) {
        BindText(idx, *value);
    } else {
        BindNull(idx);
    }
}

bool Statement::Step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    Throw(sqlite3_db_handle(stmt_), "sqlite3_step", rc);
}

void Statement::Reset() {
    const int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) {
        Throw(sqlite3_db_handle(stmt_), "sqlite3_reset", rc);
    }
    sqlite3_clear_bindings(stmt_);
}

std::int64_t Statement::ColumnInt(int idx) const {
    return sqlite3_column_int64(stmt_, idx);
}

double Statement::ColumnDouble(int idx) const {
    return sqlite3_column_double(stmt_, idx);
}

std::string Statement::ColumnText(int idx) const {
    const unsigned char* text = sqlite3_column_text(stmt_, idx);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(stmt_, idx);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(bytes));
}

bool Statement::ColumnIsNull(int idx) const {
    return sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
}

Transaction::Transaction(sqlite3* db) : db_(db) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = "sqlite3_exec(BEGIN IMMEDIATE)";
        if (err != nullptr) {
            message += ": ";
            message += err;
            sqlite3_free(err);
        }
        throw SqliteError(std::move(message), rc);
    }
}

Transaction::~Transaction() {
    if (!committed_ && db_ != nullptr) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void Transaction::Commit() {
    if (committed_) {
        return;
    }
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = "sqlite3_exec(COMMIT)";
        if (err != nullptr) {
            message += ": ";
            message += err;
            sqlite3_free(err);
        }
        throw SqliteError(std::move(message), rc);
    }
    committed_ = true;
}

Database::~Database() {
    Close();
}

void Database::Open(const std::filesystem::path& path) {
    Close();
    const std::string utf8 = path.string();
    const int rc = sqlite3_open_v2(
        utf8.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (rc != SQLITE_OK) {
        std::string message = "sqlite3_open_v2(";
        message += utf8;
        message += ")";
        if (db_ != nullptr) {
            message += ": ";
            message += sqlite3_errmsg(db_);
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw SqliteError(std::move(message), rc);
    }
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA foreign_keys=ON");
    Exec("PRAGMA synchronous=NORMAL");
}

void Database::Close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::Exec(std::string_view sql) {
    char* err = nullptr;
    const std::string sql_owned(sql);
    const int rc = sqlite3_exec(
        db_, sql_owned.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = "sqlite3_exec";
        if (err != nullptr) {
            message += ": ";
            message += err;
            sqlite3_free(err);
        }
        throw SqliteError(std::move(message), rc);
    }
}

Statement Database::Prepare(std::string_view sql) {
    return Statement(db_, sql);
}

std::int64_t Database::LastInsertRowId() const {
    return sqlite3_last_insert_rowid(db_);
}

void MigrateSchema(Database& db);

void BootstrapSchema(Database& db) {
    Transaction txn(db.handle());
    db.Exec(kSchemaSql);
    db.Exec(TickChannelSampleCreateTableSql());
    Statement upsert = db.Prepare(
        "INSERT INTO schema_meta(key, value) VALUES('schema_version', ?1) "
        "ON CONFLICT(key) DO NOTHING");
    upsert.BindText(1, std::to_string(kSchemaVersion));
    upsert.Step();
    MigrateSchema(db);
    txn.Commit();
}

int GetSchemaVersion(Database& db) {
    Statement stmt = db.Prepare(
        "SELECT value FROM schema_meta WHERE key='schema_version'");
    if (!stmt.Step()) {
        return 0;
    }
    const std::string text = stmt.ColumnText(0);
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return 0;
    }
}

bool ColumnExists(Database& db,
                  std::string_view table_name,
                  std::string_view column_name) {
    std::string sql = "PRAGMA table_info(";
    sql += std::string(table_name);
    sql += ")";
    Statement stmt = db.Prepare(sql);
    while (stmt.Step()) {
        if (stmt.ColumnText(1) == std::string(column_name)) {
            return true;
        }
    }
    return false;
}

void SetSchemaVersion(Database& db, int version) {
    Statement stmt = db.Prepare(
        "INSERT INTO schema_meta(key, value) VALUES('schema_version', ?1) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    stmt.BindText(1, std::to_string(version));
    stmt.Step();
}

void BackfillGpuEnvelope(Database& db) {
    db.Exec(R"sql(
UPDATE tick_samples
SET gpu_envelope_c =
    CASE
        WHEN gpu_core_c IS NULL
             AND gpu_memjn_c IS NULL
             AND (gpu_hotspot_c IS NULL OR gpu_hotspot_c <= 0.0)
        THEN NULL
        ELSE max(
            coalesce(gpu_core_c, -1000000.0),
            coalesce(gpu_memjn_c, -1000000.0),
            CASE
                WHEN gpu_hotspot_c IS NOT NULL AND gpu_hotspot_c > 0.0
                THEN gpu_hotspot_c
                ELSE -1000000.0
            END)
    END
WHERE gpu_envelope_c IS NULL
)sql");
}

void MigrateSchema(Database& db) {
    int version = GetSchemaVersion(db);
    if (version <= 2) {
        if (!ColumnExists(db, "tick_samples", "gpu_envelope_c")) {
            db.Exec("ALTER TABLE tick_samples ADD COLUMN gpu_envelope_c REAL");
        }
        BackfillGpuEnvelope(db);
        db.Exec(
            "CREATE INDEX IF NOT EXISTS idx_tick_samples_gpu_envelope "
            "ON tick_samples(run_id, gpu_envelope_c)");
        if (version < 2) {
            SetSchemaVersion(db, 2);
        }
    }
    if (version <= 3) {
        if (!ColumnExists(db, "tick_channel_samples",
                          "cpu_low_soak_boost_pct")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN cpu_low_soak_boost_pct REAL");
        }
        if (!ColumnExists(db, "tick_channel_samples", "response_source")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN response_source TEXT");
        }
        if (!ColumnExists(db, "tick_channel_samples", "write_reason")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN write_reason TEXT");
        }
        if (version < 3) {
            SetSchemaVersion(db, 3);
        }
    }
    if (version <= 4) {
        if (!ColumnExists(db, "tick_channel_samples",
                          "midband_pressure_boost_pct")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN midband_pressure_boost_pct REAL");
        }
        if (!ColumnExists(db, "tick_channel_samples",
                          "gpu_airflow_boost_pct")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN gpu_airflow_boost_pct REAL");
        }
        SetSchemaVersion(db, 5);
    }
    if (version <= 5) {
        if (!ColumnExists(db, "events", "severity")) {
            db.Exec("ALTER TABLE events ADD COLUMN severity TEXT");
        }
        if (!ColumnExists(db, "events", "error_code")) {
            db.Exec("ALTER TABLE events ADD COLUMN error_code TEXT");
        }
        db.Exec(
            "CREATE INDEX IF NOT EXISTS idx_events_severity "
            "ON events(severity, error_code)");
        SetSchemaVersion(db, 6);
    }
    if (version <= 6) {
        if (!ColumnExists(db, "tick_channel_samples",
                          "primary_temp_source")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN primary_temp_source TEXT");
        }
        SetSchemaVersion(db, 7);
    }
    if (version <= 7) {
        if (!ColumnExists(db, "tick_channel_samples",
                          "low_band_stage_boost_pct")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN low_band_stage_boost_pct REAL");
        }
        if (!ColumnExists(db, "tick_channel_samples",
                          "low_band_effective_boost_pct")) {
            db.Exec(
                "ALTER TABLE tick_channel_samples "
                "ADD COLUMN low_band_effective_boost_pct REAL");
        }
        SetSchemaVersion(db, 8);
    }
    if (version <= 8) {
        // FEAT-0006 (REQ-CPUEFF-02): additive nullable RAPL package-energy
        // evidence columns mirrored from the CSV (cpu_power_sample_id,
        // cpu_power_window_ms, cpu_pkg_energy_delta_uj,
        // cpu_pkg_energy_acquisition). Old archives lack them; ingest binds
        // NULL, the report derivation skips absent windows (no false zero).
        if (!ColumnExists(db, "tick_samples", "cpu_power_sample_id")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_power_sample_id "
                "INTEGER");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_power_window_ms")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_power_window_ms REAL");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_pkg_energy_delta_uj")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_pkg_energy_delta_uj "
                "REAL");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_pkg_energy_acquisition")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_pkg_energy_acquisition "
                "TEXT");
        }
        SetSchemaVersion(db, 9);
    }
    if (version <= 9) {
        // FEAT-0006 (REQ-CPUEFF-01/-03): additive nullable APERF/MPERF cycle
        // evidence columns mirrored from the CSV (cpu_cycles_sample_id,
        // cpu_cycles_window_ms, cpu_aperf_delta, cpu_mperf_delta,
        // cpu_cycles_acquisition). Old archives lack them; ingest binds NULL,
        // the report derivation skips absent windows (no false zero).
        if (!ColumnExists(db, "tick_samples", "cpu_cycles_sample_id")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_cycles_sample_id "
                "INTEGER");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_cycles_window_ms")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_cycles_window_ms "
                "REAL");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_aperf_delta")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_aperf_delta REAL");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_mperf_delta")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_mperf_delta REAL");
        }
        if (!ColumnExists(db, "tick_samples", "cpu_cycles_acquisition")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN cpu_cycles_acquisition "
                "TEXT");
        }
        SetSchemaVersion(db, 10);
    }
    if (version <= 10) {
        // FEAT-0020 (REQ-PWRLOG-02/-05): additive nullable GPU board-power
        // evidence columns mirrored from the control-loop CSV
        // (gpu_power_sample_id, gpu_power_time_ms, gpu_power_mw,
        // gpu_power_source, gpu_power_acquisition). Old archives lack them;
        // ingest binds NULL and the report summary skips absent samples (no
        // false zero). gpu_power_mw is INSTANTANEOUS board milliwatts, not an
        // accumulating energy counter, so the report derives mean/percentile,
        // not the CPU Sigma-energy/Sigma-window integral.
        if (!ColumnExists(db, "tick_samples", "gpu_power_sample_id")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN gpu_power_sample_id "
                "INTEGER");
        }
        if (!ColumnExists(db, "tick_samples", "gpu_power_time_ms")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN gpu_power_time_ms REAL");
        }
        if (!ColumnExists(db, "tick_samples", "gpu_power_mw")) {
            db.Exec("ALTER TABLE tick_samples ADD COLUMN gpu_power_mw REAL");
        }
        if (!ColumnExists(db, "tick_samples", "gpu_power_source")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN gpu_power_source TEXT");
        }
        if (!ColumnExists(db, "tick_samples", "gpu_power_acquisition")) {
            db.Exec(
                "ALTER TABLE tick_samples ADD COLUMN gpu_power_acquisition "
                "TEXT");
        }
        SetSchemaVersion(db, 11);
    }
}

}  // namespace svg_mb_control::analyze
