from __future__ import annotations

import contextlib
import datetime
import os
import sqlite3
import time

from tests.helpers import *


CSV_HEADER_PARTS = [
    "wall_clock,mode,snapshot_time,snapshot_age_ms,",
    "amd_sensor_count,amd_sensor_summary,cpu_tctl_c,cpu_max_c,",
    "gpu_available,gpu_name,gpu_last_warning,",
    "gpu_core_c,gpu_memjn_c,gpu_hotspot_c,",
    "fan_count,policy_writes_enabled_present,policy_writes_enabled,",
    # 2 fans
    "fan0_present,fan0_label,fan0_rpm,fan0_tach_raw,fan0_tach_valid,"
    "fan0_duty_raw,fan0_duty_pct,fan0_mode_raw,fan0_manual_override,"
    "fan0_write_allowed,fan0_policy_blocked,fan0_effective_write_allowed,",
    "fan1_present,fan1_label,fan1_rpm,fan1_tach_raw,fan1_tach_valid,"
    "fan1_duty_raw,fan1_duty_pct,fan1_mode_raw,fan1_manual_override,"
    "fan1_write_allowed,fan1_policy_blocked,fan1_effective_write_allowed,",
    "loop_tick_count,loop_started_wall_clock,loop_finished_wall_clock,",
    "loop_work_duration_ms,loop_intended_interval_ms,loop_achieved_interval_ms,",
    "loop_slip_ms,loop_overrun,",
    "process_cpu_delta_ms,process_cpu_pct,",
    "process_working_set_bytes,process_private_bytes,cadence_transient,",
    # 2 channels
    "channel0_observed_temp_c,channel0_setpoint_pct,"
    "channel0_thermal_pressure_boost_pct,"
    "channel0_midband_pressure_boost_pct,channel0_gpu_airflow_boost_pct,"
    "channel0_cpu_low_soak_boost_pct,"
    "channel0_response_source,channel0_write_reason,channel0_total_writes,"
    "channel0_write_active,channel0_baseline_captured,"
    "channel0_feedforward_pct,channel0_correction_pct,",
    "channel1_observed_temp_c,channel1_setpoint_pct,"
    "channel1_thermal_pressure_boost_pct,"
    "channel1_midband_pressure_boost_pct,channel1_gpu_airflow_boost_pct,"
    "channel1_cpu_low_soak_boost_pct,"
    "channel1_response_source,channel1_write_reason,channel1_total_writes,"
    "channel1_write_active,channel1_baseline_captured,"
    "channel1_feedforward_pct,channel1_correction_pct",
]
CSV_HEADER = "".join(CSV_HEADER_PARTS)


def _write_fixture_csv(path: Path, session_start: str, ticks: int = 3) -> None:
    lines = [
        "# schema=svg_mb_control.log.v1",
        "# mode=control-loop",
        f"# session_start={session_start}",
        CSV_HEADER,
    ]
    for tick in range(1, ticks + 1):
        cells = [
            session_start, "control-loop", session_start, "100",
            "1", '"Tctl/Tdie=60.000"', "60.000", "60.000",
            "true", '"NVIDIA Test GPU"', "",
            "45.000", "55.000", "0.000",
            "2", "true", "true",
            # fan0
            "true", '"Channel0"', "1500", "873", "true",
            "112", "43.92", "0", "false", "true", "false", "true",
            # fan1
            "true", '"Channel1"', "1300", "1043", "true",
            "166", "65.10", "0", "false", "true", "false", "true",
            # loop
            str(tick), session_start, session_start,
            "10.0", "50", "50.0", "0.0", "false",
            "0.0", "0.0", "33000000", "20000000", "0.0",
            # channel0
            "60.000", "30.000", "0.000", "0.500", "0.750", "0.250",
            "primary_curve+midband_pressure+gpu_airflow+cpu_low_soak",
            "first_write" if tick == 1 else "none",
            str(tick), "true", "true", "30.000", "0.000",
            # channel1
            "60.000", "32.000", "0.000", "0.000", "0.000", "0.000",
            "primary_curve", "first_write" if tick == 1 else "none",
            str(tick), "true", "true", "32.000", "0.000",
        ]
        lines.append(",".join(cells))
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
            self.assertEqual(schema[0], "6")

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
                "cpu_low_soak_boost_pct, response_source, write_reason "
                "FROM tick_channel_samples WHERE channel=0 AND tick_count=1",
            )
            self.assertEqual(attribution[0], 0.5)
            self.assertEqual(attribution[1], 0.75)
            self.assertEqual(attribution[2], 0.25)
            self.assertEqual(
                attribution[3],
                "primary_curve+midband_pressure+gpu_airflow+cpu_low_soak",
            )
            self.assertEqual(attribution[4], "first_write")

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
        first = "first_write" if tick == 1 else "none"
        cells = [
            wall, "control-loop", wall, "100",
            "1", f'"Tctl/Tdie={cpu:.3f}"', f"{cpu:.3f}", f"{cpu:.3f}",
            "true", '"NVIDIA Test GPU"', "",
            "45.000", "55.000", "0.000",
            "2", "true", "true",
            # fan0 -> channel 0 (duty tracks setpoint)
            "true", '"Channel0"', "1500", "873", "true",
            "112", f"{ch0:.2f}", "0", "false", "true", "false", "true",
            # fan1 -> channel 1
            "true", '"Channel1"', "1300", "1043", "true",
            "166", f"{ch1:.2f}", "0", "false", "true", "false", "true",
            # loop
            str(tick), wall, wall,
            "10.0", "50", "50.0", "0.0", "false",
            "0.0", "0.0", "33000000", "20000000", "0.0",
            # channel0 (total_writes cell carries the cumulative count)
            f"{cpu:.3f}", f"{ch0:.3f}", "0.000", "1.500", "0.750", "0.000",
            "primary_curve", first, str(tick), "true", "true",
            f"{ch0:.3f}", "0.000",
            # channel1
            f"{cpu:.3f}", f"{ch1:.3f}", "0.000", "0.000", "0.000", "0.000",
            "primary_curve", first, str(tick), "true", "true",
            f"{ch1:.3f}", "0.000",
        ]
        lines.append(",".join(cells))
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
