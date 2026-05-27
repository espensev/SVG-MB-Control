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


def _build_fixture(td: Path, session_start: str = "2026-05-15T03:30:00") -> Path:
    runtime_home = td / "runtime"
    logs = runtime_home / "logs"
    archive = logs / "archive"
    archive.mkdir(parents=True, exist_ok=True)

    csv_path = archive / "svg_mb_control_control-loop_20260515_033000.csv"
    manifest_path = archive / "svg_mb_control_control-loop_20260515_033000.manifest.json"
    _write_fixture_csv(csv_path, session_start)
    _write_fixture_manifest(
        manifest_path,
        session_start=session_start,
        csv_path=csv_path,
    )
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


class AnalyzeIngestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

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
            self.assertEqual(schema[0], "7")

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


class AnalyzeReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

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
                channels[0]["max_midband_pressure_boost_pct"], 1.5
            )
            self.assertEqual(channels[0]["max_gpu_airflow_boost_pct"], 0.75)

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
