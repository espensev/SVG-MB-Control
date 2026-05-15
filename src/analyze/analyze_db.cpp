#include "analyze_db.h"

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
    PRIMARY KEY (run_id, tick_count)
);

CREATE INDEX IF NOT EXISTS idx_tick_samples_wall_clock
    ON tick_samples(run_id, wall_clock);

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

CREATE TABLE IF NOT EXISTS tick_channel_samples (
    run_id INTEGER NOT NULL,
    tick_count INTEGER NOT NULL,
    channel INTEGER NOT NULL,
    observed_temp_c REAL,
    setpoint_pct REAL,
    thermal_pressure_boost_pct REAL,
    total_writes INTEGER,
    write_active INTEGER,
    baseline_captured INTEGER,
    feedforward_pct REAL,
    correction_pct REAL,
    PRIMARY KEY (run_id, tick_count, channel),
    FOREIGN KEY (run_id, tick_count)
        REFERENCES tick_samples(run_id, tick_count) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER REFERENCES runs(id) ON DELETE CASCADE,
    event_time TEXT NOT NULL,
    event_type TEXT NOT NULL,
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
    const std::string utf8 = path.u8string().empty()
                                 ? path.string()
                                 : path.string();
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

void BootstrapSchema(Database& db) {
    Transaction txn(db.handle());
    db.Exec(kSchemaSql);
    Statement upsert = db.Prepare(
        "INSERT INTO schema_meta(key, value) VALUES('schema_version', ?1) "
        "ON CONFLICT(key) DO NOTHING");
    upsert.BindText(1, std::to_string(kSchemaVersion));
    upsert.Step();
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

}  // namespace svg_mb_control::analyze
