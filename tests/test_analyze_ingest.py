from __future__ import annotations

import contextlib
import datetime
import os
import sqlite3
import time

from tests.helpers import *


COMMON_FIELDS = [
    "wall_clock",
    "mode",
    "snapshot_time",
    "snapshot_age_ms",
    "amd_sensor_count",
    "amd_sensor_summary",
    "cpu_tctl_c",
    "cpu_max_c",
    "gpu_available",
    "gpu_name",
    "gpu_last_warning",
    "gpu_core_c",
    "gpu_memjn_c",
    "gpu_hotspot_c",
    "fan_count",
    "policy_writes_enabled_present",
    "policy_writes_enabled",
]

FAN_FIELD_SUFFIXES = [
    "present",
    "label",
    "rpm",
    "tach_raw",
    "tach_valid",
    "duty_raw",
    "duty_pct",
    "mode_raw",
    "manual_override",
    "write_allowed",
    "policy_blocked",
    "effective_write_allowed",
]

LOOP_FIELDS = [
    "loop_tick_count",
    "loop_started_wall_clock",
    "loop_finished_wall_clock",
    "loop_work_duration_ms",
    "loop_intended_interval_ms",
    "loop_achieved_interval_ms",
    "loop_slip_ms",
    "loop_overrun",
    "process_cpu_delta_ms",
    "process_cpu_pct",
    "process_working_set_bytes",
    "process_private_bytes",
    "cadence_transient",
]

CHANNEL_FIELD_SUFFIXES = [
    "observed_temp_c",
    "setpoint_pct",
    "thermal_pressure_boost_pct",
    "midband_pressure_boost_pct",
    "gpu_airflow_boost_pct",
    "cpu_low_soak_boost_pct",
    "primary_temp_source",
    "response_source",
    "write_reason",
    "total_writes",
    "write_active",
    "baseline_captured",
    "feedforward_pct",
    "correction_pct",
]


def _fan_fields(channel: int) -> list[str]:
    return [f"fan{channel}_{suffix}" for suffix in FAN_FIELD_SUFFIXES]


def _channel_fields(channel: int) -> list[str]:
    return [f"channel{channel}_{suffix}" for suffix in CHANNEL_FIELD_SUFFIXES]


CSV_FIELDS = (
    COMMON_FIELDS
    + _fan_fields(0)
    + _fan_fields(1)
    + LOOP_FIELDS
    + _channel_fields(0)
    + _channel_fields(1)
)
CSV_FIELD_SET = set(CSV_FIELDS)
CSV_HEADER = ",".join(CSV_FIELDS)

# FEAT-0006 read-only RAPL package-energy columns. The base CSV_FIELDS schema
# deliberately omits them so the existing fixtures double as the "old archive
# missing new fields" case (REQ-CPUEFF-04); the energy fixture appends them.
ENERGY_FIELDS = [
    "cpu_power_sample_id",
    "cpu_power_window_ms",
    "cpu_pkg_energy_delta_uj",
    "cpu_pkg_energy_acquisition",
]
ENERGY_HEADER = ",".join(ENERGY_FIELDS)

# FEAT-0006 read-only APERF/MPERF cycle columns (schema v10), trailing the
# energy columns exactly as the runtime CSV emits them.
CYCLE_FIELDS = [
    "cpu_cycles_sample_id",
    "cpu_cycles_window_ms",
    "cpu_aperf_delta",
    "cpu_mperf_delta",
    "cpu_cycles_acquisition",
]
CYCLE_HEADER = ",".join(CYCLE_FIELDS)

# FEAT-0006 all-core effective-frequency columns (schema v13), trailing the
# per-core cycle columns. These carry the off-thread package sweep
# (Sigma-dAPERF / Sigma-dMPERF over all logical processors) on its OWN
# sample-id cadence (does not share cpu_cycles_sample_id), plus the per-sweep
# window and the contributing-core count. The base CSV_FIELDS schema omits them
# so old archive fixtures keep exercising the nullable/degrade path.
ALLCORE_FIELDS = [
    "cpu_aperf_delta_allcore",
    "cpu_mperf_delta_allcore",
    "cpu_cycles_window_ms_allcore",
    "cpu_cycles_allcore_sample_id",
    "cpu_cycles_allcore_cores",
]
ALLCORE_HEADER = ",".join(ALLCORE_FIELDS)

# FEAT-0020 read-only GPU board-power columns (schema v11), trailing the CPU
# energy/cycle columns in the power-logging fixture. The base CSV_FIELDS schema
# deliberately omits them so old archive fixtures keep testing the nullable path.
GPU_POWER_FIELDS = [
    "gpu_power_sample_id",
    "gpu_power_time_ms",
    "gpu_power_mw",
    "gpu_power_source",
    "gpu_power_acquisition",
]
GPU_POWER_HEADER = ",".join(GPU_POWER_FIELDS)

# FEAT-0021 read-only GPU workload-context columns (schema v12), trailing the
# GPU power columns. The base CSV_FIELDS schema deliberately omits them so old
# archive fixtures keep testing the nullable path.
GPU_CONTEXT_FIELDS = [
    "gpu_context_sample_id",
    "gpu_context_time_ms",
    "gpu_context_sample_age_ms",
    "gpu_context_acquisition",
    "gpu_util_gpu_pct",
    "gpu_util_mem_pct",
    "gpu_pstate",
    "gpu_clock_graphics_mhz",
    "gpu_clock_memory_mhz",
    "gpu_vram_used_mb",
    "gpu_vram_total_mb",
]
GPU_CONTEXT_HEADER = ",".join(GPU_CONTEXT_FIELDS)


def _csv_row(values: dict[str, str]) -> str:
    missing = [field for field in CSV_FIELDS if field not in values]
    extra = sorted(set(values) - CSV_FIELD_SET)
    if missing or extra:
        raise AssertionError(
            "CSV fixture field mismatch: "
            f"missing={missing!r} extra={extra!r}"
        )
    return ",".join(values[field] for field in CSV_FIELDS)


def _fan_values(
    channel: int,
    *,
    label: str,
    rpm: str,
    tach_raw: str,
    duty_raw: str,
    duty_pct: str,
) -> dict[str, str]:
    prefix = f"fan{channel}_"
    return {
        prefix + "present": "true",
        prefix + "label": f'"{label}"',
        prefix + "rpm": rpm,
        prefix + "tach_raw": tach_raw,
        prefix + "tach_valid": "true",
        prefix + "duty_raw": duty_raw,
        prefix + "duty_pct": duty_pct,
        prefix + "mode_raw": "0",
        prefix + "manual_override": "false",
        prefix + "write_allowed": "true",
        prefix + "policy_blocked": "false",
        prefix + "effective_write_allowed": "true",
    }


def _channel_values(
    channel: int,
    *,
    observed_temp_c: str,
    setpoint_pct: str,
    write_reason: str,
    total_writes: str,
    midband_pressure_boost_pct: str = "0.000",
    gpu_airflow_boost_pct: str = "0.000",
    cpu_low_soak_boost_pct: str = "0.000",
    response_source: str = "primary_curve",
) -> dict[str, str]:
    prefix = f"channel{channel}_"
    return {
        prefix + "observed_temp_c": observed_temp_c,
        prefix + "setpoint_pct": setpoint_pct,
        prefix + "thermal_pressure_boost_pct": "0.000",
        prefix + "midband_pressure_boost_pct": midband_pressure_boost_pct,
        prefix + "gpu_airflow_boost_pct": gpu_airflow_boost_pct,
        prefix + "cpu_low_soak_boost_pct": cpu_low_soak_boost_pct,
        prefix + "primary_temp_source": "cpu",
        prefix + "response_source": response_source,
        prefix + "write_reason": write_reason,
        prefix + "total_writes": total_writes,
        prefix + "write_active": "true",
        prefix + "baseline_captured": "true",
        prefix + "feedforward_pct": setpoint_pct,
        prefix + "correction_pct": "0.000",
    }


def _control_loop_fixture_row(
    *,
    wall: str,
    tick: int,
    cpu_tctl_c: float,
    fan0_duty_pct: str,
    fan1_duty_pct: str,
    channel0_setpoint_pct: str,
    channel1_setpoint_pct: str,
    channel0_midband_pressure_boost_pct: str,
    channel0_gpu_airflow_boost_pct: str,
    channel0_cpu_low_soak_boost_pct: str,
    channel0_response_source: str,
) -> str:
    cpu = f"{cpu_tctl_c:.3f}"
    write_reason = "first_write" if tick == 1 else "none"
    values = {
        "wall_clock": wall,
        "mode": "control-loop",
        "snapshot_time": wall,
        "snapshot_age_ms": "100",
        "amd_sensor_count": "1",
        "amd_sensor_summary": f'"Tctl/Tdie={cpu}"',
        "cpu_tctl_c": cpu,
        "cpu_max_c": cpu,
        "gpu_available": "true",
        "gpu_name": '"NVIDIA Test GPU"',
        "gpu_last_warning": "",
        "gpu_core_c": "45.000",
        "gpu_memjn_c": "55.000",
        "gpu_hotspot_c": "0.000",
        "fan_count": "2",
        "policy_writes_enabled_present": "true",
        "policy_writes_enabled": "true",
        "loop_tick_count": str(tick),
        "loop_started_wall_clock": wall,
        "loop_finished_wall_clock": wall,
        "loop_work_duration_ms": "10.0",
        "loop_intended_interval_ms": "50",
        "loop_achieved_interval_ms": "50.0",
        "loop_slip_ms": "0.0",
        "loop_overrun": "false",
        "process_cpu_delta_ms": "0.0",
        "process_cpu_pct": "0.0",
        "process_working_set_bytes": "33000000",
        "process_private_bytes": "20000000",
        "cadence_transient": "0.0",
    }
    values.update(
        _fan_values(
            0,
            label="Channel0",
            rpm="1500",
            tach_raw="873",
            duty_raw="112",
            duty_pct=fan0_duty_pct,
        )
    )
    values.update(
        _fan_values(
            1,
            label="Channel1",
            rpm="1300",
            tach_raw="1043",
            duty_raw="166",
            duty_pct=fan1_duty_pct,
        )
    )
    values.update(
        _channel_values(
            0,
            observed_temp_c=cpu,
            setpoint_pct=channel0_setpoint_pct,
            write_reason=write_reason,
            total_writes=str(tick),
            midband_pressure_boost_pct=channel0_midband_pressure_boost_pct,
            gpu_airflow_boost_pct=channel0_gpu_airflow_boost_pct,
            cpu_low_soak_boost_pct=channel0_cpu_low_soak_boost_pct,
            response_source=channel0_response_source,
        )
    )
    values.update(
        _channel_values(
            1,
            observed_temp_c=cpu,
            setpoint_pct=channel1_setpoint_pct,
            write_reason=write_reason,
            total_writes=str(tick),
        )
    )
    return _csv_row(values)


def _write_fixture_csv(path: Path, session_start: str, ticks: int = 3) -> None:
    lines = [
        "# schema=svg_mb_control.log.v1",
        "# mode=control-loop",
        f"# session_start={session_start}",
        CSV_HEADER,
    ]
    for tick in range(1, ticks + 1):
        lines.append(
            _control_loop_fixture_row(
                wall=session_start,
                tick=tick,
                cpu_tctl_c=60.0,
                fan0_duty_pct="43.92",
                fan1_duty_pct="65.10",
                channel0_setpoint_pct="30.000",
                channel1_setpoint_pct="32.000",
                channel0_midband_pressure_boost_pct="0.500",
                channel0_gpu_airflow_boost_pct="0.750",
                channel0_cpu_low_soak_boost_pct="0.250",
                channel0_response_source=(
                    "primary_curve+midband_pressure+gpu_airflow+cpu_low_soak"
                ),
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_fixture_manifest(
    path: Path,
    *,
    session_start: str,
    csv_path: Path,
    csv_latest_path: Path | None = None,
    status: str = "finished",
    row_count: int = 3,
    event_count: int = 5,
) -> None:
    payload = {
        "schema": "svg_mb_control.runtime_log_manifest.v1",
        "session_start": session_start,
        "mode": "control-loop",
        "status": status,
        "last_update": session_start,
        "row_count": row_count,
        "event_count": event_count,
        "producer": {
            "tool": "svg-mb-control",
            "version": "0.1.0",
            "git_hash": "deadbeef",
        },
        "writer": {
            "csv_flush_policy": "per_row",
            "mirror_mode": "write_through",
        },
        "artifacts": {
            "csv_archive": {
                "path": str(csv_path),
                "schema": "svg_mb_control.log.v1",
            },
        },
    }
    if csv_latest_path is not None:
        payload["artifacts"]["csv_latest"] = {
            "path": str(csv_latest_path),
            "schema": "svg_mb_control.log.v1",
        }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _write_fixture_events(path: Path, session_start: str) -> None:
    events = [
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.start",
            "severity": "info",
            "error_code": "none",
            "mode": "control-loop",
            "success": True,
            "detail": "control-loop started",
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.baseline_captured",
            "severity": "info",
            "error_code": "none",
            "mode": "control-loop",
            "channel": 0,
            "tick_count": 1,
            "success": True,
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.write_applied",
            "severity": "info",
            "error_code": "none",
            "mode": "control-loop",
            "channel": 0,
            "tick_count": 1,
            "observed_temp_c": 60.0,
            "setpoint_pct": 30.0,
            "success": True,
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.write_applied",
            "severity": "info",
            "error_code": "none",
            "mode": "control-loop",
            "channel": 1,
            "tick_count": 1,
            "observed_temp_c": 60.0,
            "setpoint_pct": 32.0,
            "success": True,
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.shutdown",
            "severity": "info",
            "error_code": "none",
            "mode": "control-loop",
            "success": True,
        },
    ]
    path.write_text(
        "\n".join(json.dumps(e) for e in events) + "\n",
        encoding="utf-8",
    )


def _write_fixture_plant_model(path: Path) -> None:
    payload = {
        "schema": "svg_mb_control.plant_model_capture.v1",
        "schema_version": 1,
        "captured_local": "2026-05-15T03:00:00",
        "abort_reason": None,
        "producer": {
            "tool": "svg-mb-control",
            "version": "0.1.0",
            "git_hash": "deadbeef",
        },
        "options": {
            "settle_window_ms": 100,
            "abort_temp_ceiling_c": 95.0,
            "sequence": [
                {"duty_pct": 20.0, "hold_ms": 200},
                {"duty_pct": 40.0, "hold_ms": 200},
            ],
        },
        "channels": [
            {
                "channel": 0,
                "baseline_captured": True,
                "restored": True,
                "baseline": {
                    "duty_raw": 100,
                    "mode_raw": 5,
                    "rpm": 1500.0,
                    "tctl_c": 60.0,
                    "gpu_envelope_c": None,
                },
                "steps": [
                    {
                        "duty_pct_target": 20.0,
                        "hold_ms": 200,
                        "settle_window_ms": 100,
                        "settle_sample_count": 3,
                        "duty_pct_observed_mean": 20.5,
                        "rpm_mean": 800.0,
                        "rpm_stddev": 5.0,
                        "tctl_c_mean": 60.5,
                        "gpu_envelope_c_mean": None,
                    },
                    {
                        "duty_pct_target": 40.0,
                        "hold_ms": 200,
                        "settle_window_ms": 100,
                        "settle_sample_count": 3,
                        "duty_pct_observed_mean": 40.2,
                        "rpm_mean": 1500.0,
                        "rpm_stddev": 8.0,
                        "tctl_c_mean": 60.0,
                        "gpu_envelope_c_mean": None,
                    },
                ],
            }
        ],
    }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _add_archive_fixture(
    runtime_home: Path,
    *,
    session_start: str,
    stem: str = "svg_mb_control_control-loop_20260515_033000",
    ticks: int = 3,
) -> tuple[Path, Path]:
    logs = runtime_home / "logs"
    archive = logs / "archive"
    archive.mkdir(parents=True, exist_ok=True)

    csv_path = archive / f"{stem}.csv"
    manifest_path = archive / f"{stem}.manifest.json"
    _write_fixture_csv(csv_path, session_start, ticks=ticks)
    _write_fixture_manifest(
        manifest_path,
        session_start=session_start,
        csv_path=csv_path,
        row_count=ticks,
    )
    return csv_path, manifest_path


def _build_fixture(td: Path, session_start: str = "2026-05-15T03:30:00") -> Path:
    runtime_home = td / "runtime"
    logs = runtime_home / "logs"
    _add_archive_fixture(runtime_home, session_start=session_start)
    _write_fixture_events(logs / "svg_mb_control_events.jsonl", session_start)
    _write_fixture_plant_model(runtime_home / "plant_model.json")
    return runtime_home


def _table_count(db: Path, table: str) -> int:
    with contextlib.closing(sqlite3.connect(str(db))) as conn:
        cur = conn.execute(f"SELECT COUNT(*) FROM {table}")
        return cur.fetchone()[0]


def _query_one(db: Path, sql: str, params: tuple = ()) -> tuple:
    with contextlib.closing(sqlite3.connect(str(db))) as conn:
        cur = conn.execute(sql, params)
        return cur.fetchone()


def _db_pragma_int(db: Path, pragma: str) -> int:
    with contextlib.closing(sqlite3.connect(str(db))) as conn:
        cur = conn.execute(f"PRAGMA {pragma}")
        return cur.fetchone()[0]


def _run_ingest(
    runtime_home: Path,
    db_path: Path,
    *extra: str,
) -> subprocess.CompletedProcess[str]:
    return _run_control(
        "analyze",
        "ingest",
        "--runtime-home",
        str(runtime_home),
        "--db",
        str(db_path),
        "--quiet",
        *extra,
    )


def _run_prune(
    runtime_home: Path,
    db_path: Path,
    *extra: str,
) -> subprocess.CompletedProcess[str]:
    return _run_control(
        "analyze",
        "prune",
        "--runtime-home",
        str(runtime_home),
        "--db",
        str(db_path),
        *extra,
    )


def _age_archive_bundle(runtime_home: Path, days: int = 3) -> tuple[Path, Path]:
    archive = runtime_home / "logs" / "archive"
    csv_path = archive / "svg_mb_control_control-loop_20260515_033000.csv"
    manifest_path = (
        archive / "svg_mb_control_control-loop_20260515_033000.manifest.json"
    )
    old_time = time.time() - (days * 24 * 60 * 60)
    os.utime(csv_path, (old_time, old_time))
    os.utime(manifest_path, (old_time, old_time))
    return csv_path, manifest_path


class AnalyzeIngestTests(WindowsExeTestCase):
    def test_ingest_populates_all_tables(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"

            result = _run_ingest(runtime_home, db_path)
            self.assertEqual(
                result.returncode, 0,
                msg=f"stdout={result.stdout}\nstderr={result.stderr}",
            )

            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 3)
            self.assertEqual(_table_count(db_path, "tick_fan_samples"), 6)
            self.assertEqual(_table_count(db_path, "tick_channel_samples"), 6)
            self.assertEqual(_table_count(db_path, "events"), 5)
            self.assertEqual(_table_count(db_path, "plant_model_captures"), 1)
            self.assertEqual(_table_count(db_path, "plant_model_channels"), 1)
            self.assertEqual(_table_count(db_path, "plant_model_steps"), 2)

            schema = _query_one(
                db_path,
                "SELECT value FROM schema_meta WHERE key='schema_version'",
            )
            self.assertEqual(schema[0], "13")

            run = _query_one(
                db_path,
                "SELECT mode, status, row_count_ingested, "
                "event_count_ingested, tool_version, git_hash FROM runs",
            )
            self.assertEqual(run[0], "control-loop")
            self.assertEqual(run[1], "finished")
            self.assertEqual(run[2], 3)
            self.assertEqual(run[3], 5)
            self.assertEqual(run[4], "0.1.0")
            self.assertEqual(run[5], "deadbeef")

            events_with_run = _query_one(
                db_path,
                "SELECT COUNT(*) FROM events WHERE run_id IS NOT NULL",
            )[0]
            self.assertEqual(events_with_run, 5)

            event_identity = _query_one(
                db_path,
                "SELECT severity, error_code FROM events "
                "WHERE event_type='control_loop.write_applied' LIMIT 1",
            )
            self.assertEqual(event_identity[0], "info")
            self.assertEqual(event_identity[1], "none")

            fan_count = _query_one(
                db_path,
                "SELECT COUNT(*) FROM tick_fan_samples WHERE fan_index=0",
            )[0]
            self.assertEqual(fan_count, 3)

            gpu_envelope = _query_one(
                db_path,
                "SELECT gpu_envelope_c FROM tick_samples "
                "WHERE tick_count=1",
            )[0]
            self.assertEqual(gpu_envelope, 55.0)

            channel_count = _query_one(
                db_path,
                "SELECT COUNT(*) FROM tick_channel_samples WHERE channel=1",
            )[0]
            self.assertEqual(channel_count, 3)

            attribution = _query_one(
                db_path,
                "SELECT midband_pressure_boost_pct, gpu_airflow_boost_pct, "
                "cpu_low_soak_boost_pct, primary_temp_source, "
                "response_source, write_reason "
                "FROM tick_channel_samples WHERE channel=0 AND tick_count=1",
            )
            self.assertEqual(attribution[0], 0.5)
            self.assertEqual(attribution[1], 0.75)
            self.assertEqual(attribution[2], 0.25)
            self.assertEqual(attribution[3], "cpu")
            self.assertEqual(
                attribution[4],
                "primary_curve+midband_pressure+gpu_airflow+cpu_low_soak",
            )
            self.assertEqual(attribution[5], "first_write")

            fan_label = _query_one(
                db_path,
                "SELECT label FROM tick_fan_samples "
                "WHERE fan_index=0 LIMIT 1",
            )[0]
            self.assertEqual(fan_label, "Channel0")

            step_count = _query_one(
                db_path,
                "SELECT COUNT(*) FROM plant_model_steps",
            )[0]
            self.assertEqual(step_count, 2)

    def test_ingest_is_idempotent_without_force(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"

            for _ in range(2):
                result = _run_ingest(runtime_home, db_path)
                self.assertEqual(result.returncode, 0, msg=result.stderr)

            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 3)
            self.assertEqual(_table_count(db_path, "tick_fan_samples"), 6)
            self.assertEqual(_table_count(db_path, "tick_channel_samples"), 6)
            self.assertEqual(_table_count(db_path, "plant_model_captures"), 1)

    def test_force_re_ingests_without_duplication(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"

            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)
            forced = _run_ingest(runtime_home, db_path, "--force")
            self.assertEqual(forced.returncode, 0, msg=forced.stderr)

            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 3)
            self.assertEqual(_table_count(db_path, "tick_fan_samples"), 6)
            self.assertEqual(_table_count(db_path, "tick_channel_samples"), 6)
            self.assertEqual(_table_count(db_path, "events"), 5)
            self.assertEqual(_table_count(db_path, "plant_model_captures"), 1)

    def test_live_and_archive_for_same_session_dedupe_to_one_run(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            session_start = "2026-05-15T03:30:00"
            runtime_home = _build_fixture(td, session_start=session_start)

            logs = runtime_home / "logs"
            archive_csv = (
                logs / "archive"
                / "svg_mb_control_control-loop_20260515_033000.csv"
            )
            live_manifest = logs / "svg_mb_control_manifest.json"
            _write_fixture_manifest(
                live_manifest,
                session_start=session_start,
                csv_path=archive_csv,
                status="running",
            )
            db_path = td / "svg_mb_control.db"

            result = _run_ingest(runtime_home, db_path)
            self.assertEqual(result.returncode, 0, msg=result.stderr)

            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 3)

    def test_ingest_handles_missing_runtime_home(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            missing = td / "does_not_exist"
            db_path = td / "out.db"
            result = _run_control(
                "analyze",
                "ingest",
                "--runtime-home",
                str(missing),
                "--db",
                str(db_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("runtime_home is not a directory", result.stderr)

    def test_prune_dry_run_keeps_old_ingested_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)
            csv_path, manifest_path = _age_archive_bundle(runtime_home)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "1",
                "--dry-run",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(csv_path.exists())
            self.assertTrue(manifest_path.exists())
            self.assertIn("dry_run=true", result.stdout)
            self.assertIn("candidates=1", result.stdout)
            self.assertIn("deleted=0", result.stdout)

    def test_prune_apply_deletes_old_ingested_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)
            csv_path, manifest_path = _age_archive_bundle(runtime_home)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "1",
                "--apply",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertFalse(csv_path.exists())
            self.assertFalse(manifest_path.exists())
            self.assertIn("dry_run=false", result.stdout)
            self.assertIn("candidates=1", result.stdout)
            self.assertIn("deleted=1", result.stdout)

    def test_prune_apply_skips_when_not_ingested(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"
            csv_path, manifest_path = _age_archive_bundle(runtime_home)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "1",
                "--apply",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(csv_path.exists())
            self.assertTrue(manifest_path.exists())
            self.assertIn("candidates=0", result.stdout)
            self.assertIn("skipped_not_ingested=1", result.stdout)

    def test_prune_apply_skips_running_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            session_start = "2026-05-15T03:30:00"
            runtime_home = _build_fixture(td, session_start=session_start)
            archive = runtime_home / "logs" / "archive"
            csv_path = archive / "svg_mb_control_control-loop_20260515_033000.csv"
            manifest_path = (
                archive
                / "svg_mb_control_control-loop_20260515_033000.manifest.json"
            )
            _write_fixture_manifest(
                manifest_path,
                session_start=session_start,
                csv_path=csv_path,
                status="running",
            )
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)
            _age_archive_bundle(runtime_home)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "1",
                "--apply",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(csv_path.exists())
            self.assertTrue(manifest_path.exists())
            self.assertIn("candidates=0", result.stdout)
            self.assertIn("skipped_running=1", result.stdout)

    def test_db_prune_dry_run_keeps_old_run(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "365",
                "--db-retain-days",
                "1",
                "--dry-run",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 3)
            self.assertIn("dry_run=true", result.stdout)
            self.assertIn("db_retain_days=1", result.stdout)
            self.assertIn("db_candidates=1", result.stdout)
            self.assertIn("db_deleted_runs=0", result.stdout)
            self.assertIn("db_reclaim_ran=false", result.stdout)

    def test_db_prune_apply_cascades_and_reclaims(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            logs = runtime_home / "logs"
            old_session = (
                datetime.datetime.now() - datetime.timedelta(days=10)
            ).strftime("%Y-%m-%dT%H:%M:%S")
            recent_session = datetime.datetime.now().strftime(
                "%Y-%m-%dT%H:%M:%S"
            )
            _add_archive_fixture(
                runtime_home,
                session_start=old_session,
                stem="svg_mb_control_control-loop_20260601_010000",
                ticks=800,
            )
            _add_archive_fixture(
                runtime_home,
                session_start=recent_session,
                stem="svg_mb_control_control-loop_recent",
                ticks=800,
            )
            _write_fixture_events(
                logs / "svg_mb_control_events.jsonl", old_session
            )

            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)
            self.assertEqual(_table_count(db_path, "runs"), 2)
            page_count_before = _db_pragma_int(db_path, "page_count")

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "365",
                "--db-retain-days",
                "1",
                "--apply",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("dry_run=false", result.stdout)
            self.assertIn("db_candidates=1", result.stdout)
            self.assertIn("db_deleted_runs=1", result.stdout)
            self.assertIn("db_orphan_rows=0", result.stdout)
            self.assertIn("db_reclaim_ran=true", result.stdout)

            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertEqual(_table_count(db_path, "tick_samples"), 800)
            self.assertEqual(_table_count(db_path, "tick_fan_samples"), 1600)
            self.assertEqual(_table_count(db_path, "tick_channel_samples"), 1600)
            self.assertEqual(_table_count(db_path, "events"), 0)
            retained = _query_one(
                db_path,
                "SELECT session_start FROM runs",
            )[0]
            self.assertEqual(retained, recent_session)
            self.assertLess(_db_pragma_int(db_path, "page_count"), page_count_before)
            self.assertEqual(_db_pragma_int(db_path, "freelist_count"), 0)

    def test_db_prune_zero_retain_days_is_explicit_disable(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_prune(
                runtime_home,
                db_path,
                "--retain-days",
                "365",
                "--db-retain-days",
                "0",
                "--apply",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertEqual(_table_count(db_path, "runs"), 1)
            self.assertIn("db prune disabled: db_retain_days=0", result.stdout)
            self.assertIn("db_retain_days=0", result.stdout)
            self.assertIn("db_deleted_runs=0", result.stdout)


def _ts(base_iso: str, secs: int) -> str:
    base = datetime.datetime.fromisoformat(base_iso)
    moment = base + datetime.timedelta(seconds=secs)
    return moment.strftime("%Y-%m-%dT%H:%M:%S")


def _ramp_plan(tick_index: int) -> tuple[float, float, float]:
    """Returns (cpu_tctl_c, ch0_setpoint, ch1_setpoint) for a 0-based tick.

    Ticks 0-9 are idle (cool), 10-24 are under load (hot, with a setpoint
    step at tick 12), and 25-29 are cooldown.
    """
    if tick_index < 10:
        return 50.0, 20.0, 24.0
    if tick_index < 25:
        if tick_index < 12:
            return 80.0, 20.0, 24.0
        return 80.0, 40.0, 45.0
    return 55.0, 30.0, 30.0


def _write_ramp_csv(path: Path, session_start: str, ticks: int = 30) -> None:
    lines = [
        "# schema=svg_mb_control.log.v1",
        "# mode=control-loop",
        f"# session_start={session_start}",
        CSV_HEADER,
    ]
    for i in range(ticks):
        tick = i + 1
        wall = _ts(session_start, i)
        cpu, ch0, ch1 = _ramp_plan(i)
        lines.append(
            _control_loop_fixture_row(
                wall=wall,
                tick=tick,
                cpu_tctl_c=cpu,
                fan0_duty_pct=f"{ch0:.2f}",
                fan1_duty_pct=f"{ch1:.2f}",
                channel0_setpoint_pct=f"{ch0:.3f}",
                channel1_setpoint_pct=f"{ch1:.3f}",
                channel0_midband_pressure_boost_pct="1.500",
                channel0_gpu_airflow_boost_pct="0.750",
                channel0_cpu_low_soak_boost_pct="0.000",
                channel0_response_source="primary_curve",
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_ramp_events(path: Path, session_start: str) -> None:
    events = [
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": session_start,
            "event_type": "control_loop.start",
            "mode": "control-loop",
            "success": True,
            "detail": "control-loop started",
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": _ts(session_start, 11),
            "event_type": "control_loop.authority_reasserted",
            "mode": "control-loop",
            "channel": 0,
            "tick_count": 12,
            "success": True,
            "detail": "reasserted manual authority",
        },
        {
            "schema": "svg_mb_control.event.v1",
            "event_time": _ts(session_start, 29),
            "event_type": "control_loop.shutdown",
            "mode": "control-loop",
            "success": True,
        },
    ]
    path.write_text(
        "\n".join(json.dumps(e) for e in events) + "\n",
        encoding="utf-8",
    )


def _build_report_fixture(
    td: Path, session_start: str = "2026-05-15T03:30:00"
) -> Path:
    runtime_home = td / "runtime"
    logs = runtime_home / "logs"
    archive = logs / "archive"
    archive.mkdir(parents=True, exist_ok=True)
    csv_path = archive / "svg_mb_control_control-loop_20260515_033000.csv"
    manifest_path = (
        archive / "svg_mb_control_control-loop_20260515_033000.manifest.json"
    )
    _write_ramp_csv(csv_path, session_start)
    _write_fixture_manifest(
        manifest_path,
        session_start=session_start,
        csv_path=csv_path,
        row_count=30,
        event_count=3,
    )
    _write_ramp_events(logs / "svg_mb_control_events.jsonl", session_start)
    return runtime_home


def _build_consistency_fixture(
    td: Path,
    *,
    status: str,
    manifest_is_live: bool,
    declared_rows: int,
    archive_ticks: int,
    latest_ticks: int,
    session_start: str = "2026-05-15T03:30:00",
) -> Path:
    runtime_home = td / "runtime"
    logs = runtime_home / "logs"
    archive = logs / "archive"
    archive.mkdir(parents=True, exist_ok=True)
    csv_path = archive / "svg_mb_control_control-loop_20260515_033000.csv"
    latest_path = logs / "svg_mb_control_output.csv"
    manifest_path = (
        logs / "svg_mb_control_manifest.json"
        if manifest_is_live
        else archive / "svg_mb_control_control-loop_20260515_033000.manifest.json"
    )
    _write_fixture_csv(csv_path, session_start, ticks=archive_ticks)
    _write_fixture_csv(latest_path, session_start, ticks=latest_ticks)
    _write_fixture_manifest(
        manifest_path,
        session_start=session_start,
        csv_path=csv_path,
        csv_latest_path=latest_path,
        status=status,
        row_count=declared_rows,
        event_count=0,
    )
    return runtime_home


def _run_report(
    runtime_home: Path,
    db_path: Path,
    *extra: str,
) -> subprocess.CompletedProcess[str]:
    return _run_control(
        "analyze",
        "report",
        "--runtime-home",
        str(runtime_home),
        "--db",
        str(db_path),
        *extra,
    )


# (sample_id, window_ms, delta_uj, acquisition) per tick. The logger mirrors one
# energy window across the intervening ~250 ms ticks, so each sample_id repeats
# across >=2 consecutive ticks and the report must de-duplicate on sample_id.
# Tick 1 is a baseline read (no sample id yet) and tick 8 has a blank delta (the
# log-time implausibility guard fired); both must be excluded. The 3 valid
# windows are 10 W, 30 W, 10 W -> time-weighted 60 J / 4 s = 15 W.
_ENERGY_WINDOWS = [
    ("", "", "", "quarantine"),
    ("1", "1000.000", "10000000", "quarantine"),
    ("1", "1000.000", "10000000", "quarantine"),
    ("2", "1000.000", "30000000", "quarantine"),
    ("2", "1000.000", "30000000", "quarantine"),
    ("3", "2000.000", "20000000", "quarantine"),
    ("3", "2000.000", "20000000", "quarantine"),
    ("4", "1000.000", "", "quarantine"),
]

# (sample_id, window_ms, aperf_delta, mperf_delta, acquisition) per tick,
# mirroring the energy layout: tick 1 is the baseline read, each id repeats
# across 2 ticks (de-dup on cpu_cycles_sample_id), and tick 8 is a
# guard-blanked window. Per-window ratios are 1.25, 1.5, 1.0; the
# cycle-weighted aggregate is 10750000 / 8000000 = 1.34375, deliberately NOT
# the mean-of-ratios 1.25.
_CYCLE_WINDOWS = [
    ("", "", "", "", "quarantine"),
    ("1", "1000.000", "1250000", "1000000", "quarantine"),
    ("1", "1000.000", "1250000", "1000000", "quarantine"),
    ("2", "1000.000", "7500000", "5000000", "quarantine"),
    ("2", "1000.000", "7500000", "5000000", "quarantine"),
    ("3", "2000.000", "2000000", "2000000", "quarantine"),
    ("3", "2000.000", "2000000", "2000000", "quarantine"),
    ("4", "1000.000", "", "", "quarantine"),
]

# (aperf_delta_allcore, mperf_delta_allcore, window_ms_allcore,
# allcore_sample_id, allcore_cores) per tick. The off-thread package sweep has
# its OWN cadence/sample-id, so tick 1 is its baseline (no id), each id repeats
# across 2 ticks (de-dup on cpu_cycles_allcore_sample_id, independent of the
# core-0 id), and tick 8 is a guard-blanked sweep (id present, deltas/cores
# blank). The 3 valid package windows all carry ratio 1.5 (distinct from the
# core-0 1.34375): Sigma-aperf 126e6 / Sigma-mperf 84e6 = 1.5; cores 32/32/30
# so contributing_cores max=32, min=30 makes a partial sweep auditable.
_ALLCORE_WINDOWS = [
    ("", "", "", "", ""),
    ("48000000", "32000000", "1000.000", "1", "32"),
    ("48000000", "32000000", "1000.000", "1", "32"),
    ("48000000", "32000000", "1000.000", "2", "32"),
    ("48000000", "32000000", "1000.000", "2", "32"),
    ("30000000", "20000000", "2000.000", "3", "30"),
    ("30000000", "20000000", "2000.000", "3", "30"),
    ("", "", "1000.000", "4", ""),
]

# (sample_id, time_ms, mw, source, acquisition) per tick. sample ids repeat to
# prove the report de-duplicates mirrored/cached rows. Tick 1 is unavailable, and
# tick 8 has a sample id but blank mw; neither may become a false zero.
_GPU_POWER_SAMPLES = [
    ("", "", "", "unknown", "unavailable"),
    ("1", "100.000", "250000", "nvml", "nvml"),
    ("1", "100.000", "250000", "nvml", "nvml"),
    ("2", "150.000", "300000", "nvml", "nvml"),
    ("2", "150.000", "300000", "nvml", "nvml"),
    ("3", "200.000", "150000", "nvml", "nvml"),
    ("3", "200.000", "150000", "nvml", "nvml"),
    ("4", "250.000", "", "nvml", "nvml"),
]

# (sample_id, time_ms, age_ms, acquisition, util_gpu, util_mem, pstate,
# graphics_clock, memory_clock, vram_used, vram_total) per tick. Sample ids
# repeat to prove report de-duplicates cached context rows while age remains
# row-based.
_GPU_CONTEXT_SAMPLES = [
    ("", "", "", "unavailable", "", "", "", "", "", "", ""),
    ("1", "1000.000", "0.000", "nvml", "20", "10", "2", "1000", "8000", "2048", "16384"),
    ("1", "1000.000", "250.000", "nvml", "20", "10", "2", "1000", "8000", "2048", "16384"),
    ("2", "2000.000", "0.000", "nvml", "80", "40", "0", "2500", "10500", "8192", "16384"),
    ("2", "2000.000", "250.000", "nvml", "80", "40", "0", "2500", "10500", "8192", "16384"),
    ("3", "3000.000", "0.000", "nvml", "60", "30", "0", "2300", "10000", "6144", "16384"),
    ("3", "3000.000", "250.000", "nvml", "60", "30", "0", "2300", "10000", "6144", "16384"),
    ("4", "4000.000", "0.000", "nvml", "", "", "", "", "", "", ""),
]


def _write_energy_csv(path: Path, session_start: str) -> None:
    lines = [
        "# schema=svg_mb_control.log.v1",
        "# mode=control-loop",
        f"# session_start={session_start}",
        CSV_HEADER + "," + ENERGY_HEADER + "," + CYCLE_HEADER + ","
        + ALLCORE_HEADER + "," + GPU_POWER_HEADER + "," + GPU_CONTEXT_HEADER,
    ]
    for i, (energy, cycles, allcore, gpu_power, gpu_context) in enumerate(
        zip(
            _ENERGY_WINDOWS,
            _CYCLE_WINDOWS,
            _ALLCORE_WINDOWS,
            _GPU_POWER_SAMPLES,
            _GPU_CONTEXT_SAMPLES,
        )
    ):
        base = _control_loop_fixture_row(
            wall=_ts(session_start, i),
            tick=i + 1,
            cpu_tctl_c=60.0,
            fan0_duty_pct="43.92",
            fan1_duty_pct="65.10",
            channel0_setpoint_pct="30.000",
            channel1_setpoint_pct="32.000",
            channel0_midband_pressure_boost_pct="0.000",
            channel0_gpu_airflow_boost_pct="0.000",
            channel0_cpu_low_soak_boost_pct="0.000",
            channel0_response_source="primary_curve",
        )
        lines.append(
            base + "," + ",".join(energy) + "," + ",".join(cycles)
            + "," + ",".join(allcore)
            + "," + ",".join(gpu_power)
            + "," + ",".join(gpu_context)
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _build_energy_fixture(
    td: Path, session_start: str = "2026-05-15T04:00:00"
) -> Path:
    runtime_home = td / "runtime"
    logs = runtime_home / "logs"
    archive = logs / "archive"
    archive.mkdir(parents=True, exist_ok=True)
    csv_path = archive / "svg_mb_control_control-loop_20260515_040000.csv"
    manifest_path = (
        archive / "svg_mb_control_control-loop_20260515_040000.manifest.json"
    )
    _write_energy_csv(csv_path, session_start)
    _write_fixture_manifest(
        manifest_path,
        session_start=session_start,
        csv_path=csv_path,
        row_count=len(_ENERGY_WINDOWS),
        event_count=5,
    )
    _write_fixture_events(logs / "svg_mb_control_events.jsonl", session_start)
    return runtime_home


class AnalyzeReportTests(WindowsExeTestCase):
    def test_report_text_summarises_bands_and_response(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_report_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "10"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            out = result.stdout
            self.assertIn("analyze report: run_id=", out)
            self.assertIn(
                "band sample counts: idle=10 load=15 cooldown=5", out
            )
            self.assertIn("[idle] cpu_tctl_c p50=50.000", out)
            self.assertIn("[load] cpu_tctl_c p50=80.000 p90=80.000", out)
            self.assertIn("[cooldown] cpu_tctl_c p50=55.000", out)
            self.assertIn("response_delay_s=2.000", out)
            self.assertIn("authority_reasserted=1", out)
            self.assertIn("ch0 setpoint_pct", out)
            self.assertIn("ch1 setpoint_pct", out)
            self.assertIn("midband_pressure_boost_pct max=1.5", out)
            self.assertIn("gpu_airflow_boost_pct max=0.75", out)
            self.assertIn("primary_temp_source_counts cpu=30", out)
            self.assertIn("response_source_counts primary_curve=30", out)
            self.assertIn("write_reason_counts first_write=1 none=29", out)
            self.assertIn("events: severity_counts=unknown=3", out)

    def test_report_json_emits_structured_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_report_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "10", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            self.assertEqual(obj["bands"]["idle"]["n"], 10)
            self.assertEqual(obj["bands"]["load"]["n"], 15)
            self.assertEqual(obj["bands"]["cooldown"]["n"], 5)
            self.assertEqual(obj["bands"]["load"]["cpu_tctl_c"]["max"], 80.0)
            self.assertEqual(obj["bands"]["idle"]["cpu_tctl_c"]["p50"], 50.0)
            self.assertEqual(obj["response"]["load_onset_tick"], 11)
            self.assertEqual(obj["response"]["response_delay_s"], 2.0)
            self.assertEqual(obj["robustness"]["authority_reasserted"], 1)
            self.assertEqual(obj["robustness"]["write_failures"], 0)
            channels = {c["channel"]: c for c in obj["channels"]}
            self.assertEqual(set(channels), {0, 1})
            self.assertGreaterEqual(channels[0]["reversals"], 1)
            self.assertEqual(channels[0]["mode_leave_ticks"], 0)
            self.assertEqual(channels[0]["writes"], 29)
            self.assertEqual(
                channels[0]["primary_temp_source_counts"]["cpu"], 30
            )
            self.assertEqual(
                channels[0]["response_source_counts"]["primary_curve"], 30
            )
            self.assertEqual(channels[0]["write_reason_counts"]["none"], 29)
            self.assertEqual(
                channels[0]["max_midband_pressure_boost_pct"], 1.5
            )
            self.assertEqual(channels[0]["max_gpu_airflow_boost_pct"], 0.75)
            # Phase B report sections: GPU-envelope peak, loop-timing and
            # process-resource percentiles, and the low-band-inclusive
            # response-boost total (the ramp fixture holds these constant).
            self.assertIn("response_boost_total_pct", channels[0])
            self.assertEqual(obj["gpu_response"]["peak"]["value_c"], 55.0)
            self.assertEqual(
                obj["timing"]["loop_achieved_interval_ms"]["p50"], 50.0
            )
            self.assertEqual(
                obj["timing"]["loop_achieved_interval_ms"]["max"], 50.0
            )
            self.assertEqual(obj["timing"]["overrun_count"], 0)
            self.assertEqual(
                obj["resources"]["process_working_set_bytes"]["max"], 33000000
            )
            self.assertEqual(obj["events"]["severity_counts"]["unknown"], 3)
            self.assertEqual(obj["events"]["error_code_counts"]["none"], 3)
            self.assertEqual(
                obj["diagnostic_flags"],
                ["authority_reasserted_during_run"],
            )
            # FEAT-0006: the ramp fixture carries no energy columns, so the
            # derivation must report unavailable -- avg_watts null, NOT a false
            # zero -- and surface why via the acquisition provenance.
            self.assertEqual(obj["package_power"]["window_count"], 0)
            self.assertIsNone(obj["package_power"]["avg_watts"])
            self.assertEqual(
                obj["package_power"]["acquisition_counts"]["unavailable"], 30
            )
            # Same no-false-zero contract for the cycle block (no cycle
            # columns in the ramp fixture either).
            self.assertEqual(obj["cpu_cycles"]["window_count"], 0)
            self.assertIsNone(obj["cpu_cycles"]["aperf_mperf_ratio"])
            self.assertIsNone(obj["cpu_cycles"]["effective_mhz"])
            self.assertEqual(
                obj["cpu_cycles"]["acquisition_counts"]["unavailable"], 30
            )
            # Same no-false-zero contract for GPU power on old archives without
            # FEAT-0020 columns.
            self.assertEqual(obj["gpu_power"]["sample_count"], 0)
            self.assertIsNone(obj["gpu_power"]["avg_mw"])
            self.assertEqual(
                obj["gpu_power"]["acquisition_counts"]["unavailable"], 30
            )
            self.assertEqual(obj["gpu_context"]["sample_count"], 0)
            self.assertEqual(obj["gpu_context"]["util_gpu_pct"]["count"], 0)
            self.assertEqual(
                obj["gpu_context"]["acquisition_counts"]["unavailable"], 30
            )

    def test_report_warns_on_running_csv_manifest_count_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_consistency_fixture(
                td,
                status="running",
                manifest_is_live=True,
                declared_rows=10,
                archive_ticks=3,
                latest_ticks=2,
            )
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            self.assertEqual(obj["run"]["row_count_declared"], 10)
            self.assertEqual(obj["run"]["row_count_ingested"], 3)
            self.assertEqual(obj["run"]["csv_latest_row_count"], 2)
            self.assertIn(
                "running_csv_manifest_consistency_warning",
                obj["diagnostic_flags"],
            )
            self.assertNotIn(
                "closed_csv_manifest_consistency_suspect_evidence",
                obj["diagnostic_flags"],
            )

            text = _run_report(
                runtime_home, db_path, "--idle-seconds", "1"
            ).stdout
            self.assertIn("rows declared/ingested=10/3 latest=2", text)
            self.assertIn("running_csv_manifest_consistency_warning", text)

    def test_report_marks_closed_csv_manifest_mismatch_suspect(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_consistency_fixture(
                td,
                status="completed",
                manifest_is_live=False,
                declared_rows=10,
                archive_ticks=3,
                latest_ticks=3,
            )
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            flags = json.loads(result.stdout)["diagnostic_flags"]
            self.assertIn(
                "closed_csv_manifest_consistency_suspect_evidence",
                flags,
            )
            self.assertNotIn("running_csv_manifest_consistency_warning", flags)

    def test_report_derives_time_weighted_package_power(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            pp = obj["package_power"]
            # 8 ticks carry 4 sample-ids, but the baseline (tick 1, no id) and
            # the blank-delta window (sample_id 4, tick 8) are excluded -> 3
            # distinct de-duplicated windows, not 8 tick rows and not 4 ids.
            self.assertEqual(pp["window_count"], 3)
            self.assertEqual(pp["total_energy_j"], 60.0)
            self.assertEqual(pp["total_window_s"], 4.0)
            # Time-weighted 60 J / 4 s = 15 W, NOT mean-of-means (10+30+10)/3.
            self.assertEqual(pp["avg_watts"], 15.0)
            self.assertEqual(pp["watts"]["max"], 30.0)
            self.assertEqual(pp["acquisition_counts"]["quarantine"], 8)

            text = _run_report(
                runtime_home, db_path, "--idle-seconds", "1"
            ).stdout
            self.assertIn(
                "package_power: windows=3 avg_watts=15.000", text
            )

    def test_report_derives_gpu_power_distribution(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            row = _query_one(
                db_path,
                "SELECT gpu_power_sample_id, gpu_power_time_ms, gpu_power_mw, "
                "gpu_power_source, gpu_power_acquisition FROM tick_samples "
                "WHERE tick_count=2",
            )
            self.assertEqual(row[0], 1)
            self.assertEqual(row[1], 100.0)
            self.assertEqual(row[2], 250000.0)
            self.assertEqual(row[3], "nvml")
            self.assertEqual(row[4], "nvml")

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            gp = json.loads(result.stdout)["gpu_power"]
            # 8 ticks carry 4 sample ids, but sample id 4 has blank mw and ids
            # 1-3 are mirrored across ticks -> 3 distinct instantaneous samples.
            self.assertEqual(gp["sample_count"], 3)
            self.assertAlmostEqual(gp["avg_mw"], 233333.33333333334)
            self.assertEqual(gp["mw"]["p50"], 250000.0)
            self.assertEqual(gp["mw"]["p90"], 300000.0)
            self.assertEqual(gp["mw"]["max"], 300000.0)
            self.assertEqual(gp["acquisition_counts"]["nvml"], 7)
            self.assertEqual(gp["acquisition_counts"]["unavailable"], 1)

            text = _run_report(
                runtime_home, db_path, "--idle-seconds", "1"
            ).stdout
            self.assertIn("gpu_power: samples=3 avg_mw=233333.333", text)

    def test_report_derives_gpu_context_distribution(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            row = _query_one(
                db_path,
                "SELECT gpu_context_sample_id, gpu_context_time_ms, "
                "gpu_context_sample_age_ms, gpu_context_acquisition, "
                "gpu_util_gpu_pct, gpu_util_mem_pct, gpu_pstate, "
                "gpu_clock_graphics_mhz, gpu_clock_memory_mhz, "
                "gpu_vram_used_mb, gpu_vram_total_mb FROM tick_samples "
                "WHERE tick_count=4",
            )
            self.assertEqual(row[0], 2)
            self.assertEqual(row[1], 2000.0)
            self.assertEqual(row[2], 0.0)
            self.assertEqual(row[3], "nvml")
            self.assertEqual(row[4], 80)
            self.assertEqual(row[5], 40)
            self.assertEqual(row[6], 0)
            self.assertEqual(row[7], 2500)
            self.assertEqual(row[8], 10500)
            self.assertEqual(row[9], 8192)
            self.assertEqual(row[10], 16384)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            gc = json.loads(result.stdout)["gpu_context"]
            # 8 ticks carry 4 context sample ids, but sample id 4 has all
            # workload values blank. It still counts as a context identity;
            # metric distributions skip each blank value independently.
            self.assertEqual(gc["sample_count"], 4)
            self.assertEqual(gc["util_gpu_pct"]["p50"], 60.0)
            self.assertEqual(gc["util_gpu_pct"]["p90"], 80.0)
            self.assertEqual(gc["util_mem_pct"]["p50"], 30.0)
            self.assertEqual(gc["pstate_counts"]["0"], 2)
            self.assertEqual(gc["pstate_counts"]["2"], 1)
            self.assertEqual(gc["clock_graphics_mhz"]["max"], 2500.0)
            self.assertEqual(gc["clock_memory_mhz"]["p50"], 10000.0)
            self.assertEqual(gc["vram_used_mb"]["p90"], 8192.0)
            self.assertEqual(gc["vram_total_mb"]["p50"], 16384.0)
            self.assertEqual(gc["sample_age_ms"]["max"], 250.0)
            self.assertEqual(gc["acquisition_counts"]["nvml"], 7)
            self.assertEqual(gc["acquisition_counts"]["unavailable"], 1)

            text = _run_report(
                runtime_home, db_path, "--idle-seconds", "1"
            ).stdout
            self.assertIn("gpu_context: samples=4", text)
            self.assertIn("util_gpu_pct p50=60.000", text)

    def test_report_derives_cycle_ratio_and_effective_frequency(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            cc = json.loads(result.stdout)["cpu_cycles"]
            # 8 ticks carry 4 sample-ids, but the baseline (tick 1, no id) and
            # the guard-blanked window (sample_id 4, tick 8) are excluded -> 3
            # distinct de-duplicated windows, not 8 tick rows and not 4 ids.
            self.assertEqual(cc["window_count"], 3)
            self.assertEqual(cc["total_aperf_cycles"], 10750000.0)
            self.assertEqual(cc["total_mperf_cycles"], 8000000.0)
            self.assertEqual(cc["total_window_s"], 4.0)
            # Cycle-weighted 10750000/8000000 = 1.34375, NOT the
            # mean-of-per-window-ratios (1.25 + 1.5 + 1.0)/3.
            self.assertEqual(cc["aperf_mperf_ratio"], 1.34375)
            self.assertEqual(cc["ratio"]["p50"], 1.25)
            self.assertEqual(cc["ratio"]["max"], 1.5)
            self.assertEqual(cc["acquisition_counts"]["quarantine"], 8)
            # Without an operator-supplied P0 there is no effective MHz --
            # no document fixes a P0 source, so the report must not guess.
            self.assertIsNone(cc["effective_mhz"])
            self.assertIsNone(cc["p0_mhz"])

            with_p0 = _run_report(
                runtime_home, db_path,
                "--idle-seconds", "1", "--p0-mhz", "4000", "--json",
            )
            self.assertEqual(with_p0.returncode, 0, msg=with_p0.stderr)
            cc = json.loads(with_p0.stdout)["cpu_cycles"]
            self.assertEqual(cc["p0_mhz"], 4000.0)
            # effective = aggregate ratio 1.34375 x 4000 MHz.
            self.assertEqual(cc["effective_mhz"], 5375.0)

            text = _run_report(
                runtime_home, db_path,
                "--idle-seconds", "1", "--p0-mhz", "4000",
            ).stdout
            self.assertIn(
                "cpu_cycles: windows=3 aperf_mperf_ratio=1.344 "
                "effective_mhz=5375.000 p0_mhz=4000.000",
                text,
            )

    def test_report_derives_allcore_cycle_ratio_and_cores(self) -> None:
        # FEAT-0006 all-core rollup: the off-thread package sweep is reported as
        # a SECOND cpu_cycles_allcore block, de-duplicated on its OWN
        # cpu_cycles_allcore_sample_id (independent of the core-0 cpu_cycles
        # block), with the contributing-core count exposed so a partial sweep is
        # auditable. Package ratio 1.5 is deliberately distinct from the core-0
        # 1.34375 to prove the two streams do not merge.
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            ac = obj["cpu_cycles_allcore"]
            # 3 distinct package windows (ids 1,2,3); the baseline (tick 1, no
            # id) and the guard-blanked sweep (id 4, tick 8) are excluded -> 3
            # de-duplicated windows, not 8 tick rows.
            self.assertEqual(ac["window_count"], 3)
            self.assertEqual(ac["total_aperf_cycles"], 126000000.0)
            self.assertEqual(ac["total_mperf_cycles"], 84000000.0)
            self.assertEqual(ac["total_window_s"], 4.0)
            # Package ratio 126e6 / 84e6 = 1.5, distinct from core-0 1.34375.
            self.assertEqual(ac["aperf_mperf_ratio"], 1.5)
            # The core-0 block is still derived independently and unchanged.
            self.assertEqual(obj["cpu_cycles"]["aperf_mperf_ratio"], 1.34375)
            # Contributing cores: full sweeps 32, the partial window 30.
            self.assertEqual(ac["contributing_cores_max"], 32)
            self.assertEqual(ac["contributing_cores_min"], 30)
            # No operator P0 -> no effective MHz, never a guessed base.
            self.assertIsNone(ac["effective_mhz"])
            self.assertIsNone(ac["p0_mhz"])

            with_p0 = _run_report(
                runtime_home, db_path,
                "--idle-seconds", "1", "--p0-mhz", "4000", "--json",
            )
            self.assertEqual(with_p0.returncode, 0, msg=with_p0.stderr)
            ac = json.loads(with_p0.stdout)["cpu_cycles_allcore"]
            self.assertEqual(ac["p0_mhz"], 4000.0)
            # effective = package ratio 1.5 x 4000 MHz.
            self.assertEqual(ac["effective_mhz"], 6000.0)

            text = _run_report(
                runtime_home, db_path,
                "--idle-seconds", "1", "--p0-mhz", "4000",
            ).stdout
            self.assertIn(
                "cpu_cycles_allcore: windows=3 aperf_mperf_ratio=1.500 "
                "effective_mhz=6000.000 p0_mhz=4000.000",
                text,
            )
            self.assertIn("cores_max=32", text)
            self.assertIn("cores_min=30", text)

    def test_report_degrades_when_cycle_columns_missing(self) -> None:
        # A v9-shaped DB read by the current binary: the cycle query references
        # columns that do not exist and throws "no such column". The single try/catch
        # in SummariseCpuCycles must degrade that to an "unavailable" block
        # (no false zero) while the v9 package-power block still derives.
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            with contextlib.closing(sqlite3.connect(str(db_path))) as conn:
                for col in (
                    "cpu_cycles_sample_id",
                    "cpu_cycles_window_ms",
                    "cpu_aperf_delta",
                    "cpu_mperf_delta",
                    "cpu_cycles_acquisition",
                ):
                    conn.execute(
                        f"ALTER TABLE tick_samples DROP COLUMN {col}"
                    )
                conn.execute(
                    "UPDATE schema_meta SET value='9' WHERE key='schema_version'"
                )
                conn.commit()

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            self.assertEqual(obj["cpu_cycles"]["window_count"], 0)
            self.assertIsNone(obj["cpu_cycles"]["aperf_mperf_ratio"])
            # The energy columns are still present, so package power is
            # unaffected by the missing cycle columns.
            self.assertEqual(obj["package_power"]["avg_watts"], 15.0)

    def test_report_degrades_when_allcore_columns_missing(self) -> None:
        # A pre-v13 DB read by the current binary: the all-core columns do not
        # exist, so SummariseCpuCyclesAllcore's query throws "no such column".
        # Its OWN try/catch must degrade only the all-core block to
        # "unavailable" (no false zero) while the per-core cpu_cycles block --
        # which uses a SEPARATE query with no all-core filter -- still derives.
        # This is the filter-coupling regression guard: the two cycle streams
        # must not share a WHERE clause.
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            with contextlib.closing(sqlite3.connect(str(db_path))) as conn:
                for col in ALLCORE_FIELDS:
                    conn.execute(
                        f"ALTER TABLE tick_samples DROP COLUMN {col}"
                    )
                conn.execute(
                    "UPDATE schema_meta SET value='12' WHERE key='schema_version'"
                )
                conn.commit()

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            obj = json.loads(result.stdout)
            # All-core block degrades to unavailable -- never a false zero.
            self.assertEqual(obj["cpu_cycles_allcore"]["window_count"], 0)
            self.assertIsNone(obj["cpu_cycles_allcore"]["aperf_mperf_ratio"])
            self.assertIsNone(
                obj["cpu_cycles_allcore"]["contributing_cores_max"]
            )
            self.assertIsNone(
                obj["cpu_cycles_allcore"]["contributing_cores_min"]
            )
            # The per-core block is UNAFFECTED: separate query, no shared filter.
            self.assertEqual(obj["cpu_cycles"]["window_count"], 3)
            self.assertEqual(obj["cpu_cycles"]["aperf_mperf_ratio"], 1.34375)

    def test_ingest_migrates_v9_db_to_current_schema(self) -> None:
        # A pre-existing v9 DB picked up by this binary: ingest bootstraps
        # (BootstrapSchema -> MigrateSchema) before its strict version check,
        # so the cycle and GPU-power columns are added, the version moves to the
        # current schema head, and forced re-ingest backfills the evidence data.
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            with contextlib.closing(sqlite3.connect(str(db_path))) as conn:
                for col in CYCLE_FIELDS + GPU_POWER_FIELDS + GPU_CONTEXT_FIELDS:
                    conn.execute(
                        f"ALTER TABLE tick_samples DROP COLUMN {col}"
                    )
                conn.execute(
                    "UPDATE schema_meta SET value='9' WHERE key='schema_version'"
                )
                conn.commit()

            self.assertEqual(
                _run_ingest(runtime_home, db_path, "--force").returncode, 0
            )
            with contextlib.closing(sqlite3.connect(str(db_path))) as conn:
                version = conn.execute(
                    "SELECT value FROM schema_meta WHERE key='schema_version'"
                ).fetchone()[0]
            # The migration ladder runs to the current head: v9->v10 adds the
            # cycle columns, v10->v11 adds FEAT-0020 GPU power, v11->v12 adds
            # FEAT-0021 GPU workload context, and v12->v13 adds the FEAT-0006
            # all-core effective-frequency columns.
            self.assertEqual(version, "13")

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            cc = json.loads(result.stdout)["cpu_cycles"]
            self.assertEqual(cc["window_count"], 3)
            self.assertEqual(cc["aperf_mperf_ratio"], 1.34375)
            gp = json.loads(result.stdout)["gpu_power"]
            self.assertEqual(gp["sample_count"], 3)
            self.assertEqual(gp["mw"]["max"], 300000.0)
            gc = json.loads(result.stdout)["gpu_context"]
            self.assertEqual(gc["sample_count"], 4)
            self.assertEqual(gc["clock_graphics_mhz"]["max"], 2500.0)

    def test_report_degrades_when_energy_columns_missing(self) -> None:
        # An old (schema-8) DB read by a schema-9 binary: report.cpp checks only
        # that a schema is present (version > 0), not that it matches, and does
        # not migrate -- so the package-power query references columns that do
        # not exist and throws "no such column". The single try/catch in
        # SummarisePackagePower must degrade that to an "unavailable" block (no
        # false zero) without failing the whole report.
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_energy_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            # Downgrade the freshly-ingested v9 DB to look like a v8 one: drop the
            # four additive cpu_power_* columns and reset the recorded version.
            with contextlib.closing(sqlite3.connect(str(db_path))) as conn:
                for col in (
                    "cpu_power_sample_id",
                    "cpu_power_window_ms",
                    "cpu_pkg_energy_delta_uj",
                    "cpu_pkg_energy_acquisition",
                ):
                    conn.execute(
                        f"ALTER TABLE tick_samples DROP COLUMN {col}"
                    )
                conn.execute(
                    "UPDATE schema_meta SET value='8' WHERE key='schema_version'"
                )
                conn.commit()

            result = _run_report(
                runtime_home, db_path, "--idle-seconds", "1", "--json"
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            pp = json.loads(result.stdout)["package_power"]
            # Missing columns -> query throws -> empty (unavailable) summary,
            # not a crash and not a false zero.
            self.assertEqual(pp["window_count"], 0)
            self.assertIsNone(pp["avg_watts"])

            text = _run_report(
                runtime_home, db_path, "--idle-seconds", "1"
            ).stdout
            self.assertIn("package_power: windows=0", text)

    def test_report_writes_native_analysis_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_report_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            out_path = td / "analysis" / "combined-load-summary.txt"
            manifest_path = td / "analysis" / "combined-load-manifest.json"
            result = _run_report(
                runtime_home,
                db_path,
                "--idle-seconds",
                "10",
                "--out",
                str(out_path),
                "--manifest-out",
                str(manifest_path),
                "--profile",
                "combined-load",
                "--hypothesis",
                "fan response rises under load",
                "--decision",
                "keep",
                "--notes",
                "fixture notes",
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)

            decision_path = out_path.with_name(
                out_path.stem + ".decision.md"
            )
            self.assertTrue(out_path.exists())
            self.assertTrue(decision_path.exists())
            self.assertTrue(manifest_path.exists())

            report = out_path.read_text(encoding="utf-8")
            self.assertIn("analyze report: run_id=", report)
            self.assertIn("response_source_counts primary_curve=30", report)

            decision_record = decision_path.read_text(encoding="utf-8")
            self.assertIn("# Control Run Decision Record", decision_record)
            self.assertIn("Profile: combined-load", decision_record)
            self.assertIn("authority_reasserted_during_run", decision_record)
            self.assertRegex(decision_record, r"CSV: .* sha256=[0-9a-f]{64}")

            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                manifest["schema"],
                "svg_mb_control.analysis_manifest.v1",
            )
            self.assertEqual(manifest["params"]["profile"], "combined-load")
            self.assertEqual(manifest["run"]["row_count_ingested"], 30)
            self.assertRegex(
                manifest["source_artifacts"]["csv"]["sha256"],
                r"^[0-9a-f]{64}$",
            )
            self.assertRegex(
                manifest["source_artifacts"]["runtime_manifest"]["sha256"],
                r"^[0-9a-f]{64}$",
            )
            self.assertRegex(
                manifest["outputs"]["report"]["sha256"],
                r"^[0-9a-f]{64}$",
            )
            self.assertRegex(
                manifest["outputs"]["decision_record"]["sha256"],
                r"^[0-9a-f]{64}$",
            )

    def test_report_run_selection_and_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = _build_report_fixture(td)
            db_path = td / "svg_mb_control.db"
            self.assertEqual(_run_ingest(runtime_home, db_path).returncode, 0)

            session = "2026-05-15T03:30:00"
            ok = _run_report(
                runtime_home, db_path, "--session", session,
                "--idle-seconds", "10",
            )
            self.assertEqual(ok.returncode, 0, msg=ok.stderr)
            self.assertIn("session_start=" + session, ok.stdout)

            missing_run = _run_report(
                runtime_home, db_path, "--run", "9999"
            )
            self.assertEqual(missing_run.returncode, 1)
            self.assertIn("no matching run", missing_run.stderr)

            absent_db = _run_report(runtime_home, td / "absent.db")
            self.assertEqual(absent_db.returncode, 1)
            self.assertIn("does not exist", absent_db.stderr)

            invalid_numbers = [
                ("--run", "-1"),
                ("--run", "1x"),
                ("--idle-seconds", "10s"),
                ("--load-threshold-c", "70c"),
                ("--gpu-load-threshold-c", "66c"),
                ("--p0-mhz", "4000x"),
            ]
            for flag, value in invalid_numbers:
                with self.subTest(flag=flag, value=value):
                    result = _run_report(runtime_home, db_path, flag, value)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(f"invalid {flag} value", result.stderr)
