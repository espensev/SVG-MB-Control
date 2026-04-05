from __future__ import annotations

import ctypes
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build" / "x64-release"
CONTROL_EXE = BUILD_DIR / "svg-mb-control.exe"
FAKE_BENCH_EXE = BUILD_DIR / "fake-bench.exe"
REAL_BENCH_EXE = REPO_ROOT.parent / "SVG-MB-Bench" / "svg-mb-bench.exe"
FAKE_SNAPSHOT = BUILD_DIR / "fake_logger_service_current_state.json"


def _has_build_tools() -> bool:
    return shutil.which("cmake") is not None and shutil.which("ninja") is not None


def _ensure_release_build() -> None:
    if CONTROL_EXE.is_file() and FAKE_BENCH_EXE.is_file():
        return

    if not _has_build_tools():
        raise unittest.SkipTest("cmake or ninja is not available")

    configure = subprocess.run(
        ["cmake", "--preset", "x64-release"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if configure.returncode != 0:
        raise unittest.SkipTest(
            "cmake configure failed:\n"
            f"{configure.stdout}\n{configure.stderr}"
        )

    build = subprocess.run(
        ["cmake", "--build", "--preset", "x64-release"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        raise unittest.SkipTest(
            "cmake build failed:\n"
            f"{build.stdout}\n{build.stderr}"
        )


def _run_control(*args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        [str(CONTROL_EXE), *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        env=merged_env,
    )


def _is_elevated() -> bool:
    if sys.platform != "win32":
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


class SmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_default_logger_service_outputs_json(self) -> None:
        result = _run_control("--bench-exe-path", str(FAKE_BENCH_EXE))
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")

        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["command"], "logger-service")
        self.assertEqual(data["mode"], "success")
        self.assertEqual(data["duration_ms"], 1000)
        self.assertEqual(data["sample"], 1)

    def test_read_snapshot_mode_outputs_json(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            "--bridge-command",
            "read-snapshot",
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")

        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["command"], "read-snapshot")
        self.assertEqual(data["duration_ms"], 0)

    def test_logger_service_missing_snapshot_path_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            env={"SVG_MB_CONTROL_FAKE_MODE": "missing_snapshot_path"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("snapshot_path", result.stderr)

    def test_read_snapshot_missing_snapshot_archive_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            "--bridge-command",
            "read-snapshot",
            env={"SVG_MB_CONTROL_FAKE_MODE": "missing_snapshot_path"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("snapshot_archive", result.stderr)

    def test_logger_service_missing_snapshot_file_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            env={"SVG_MB_CONTROL_FAKE_MODE": "missing_snapshot_file"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("JSON file not found", result.stderr)

    def test_logger_service_nonzero_exit_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            env={"SVG_MB_CONTROL_FAKE_MODE": "fail_exit"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("logger-service exited", result.stderr)

    def test_logger_service_duration_override_flows_to_bench(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            "--duration-ms",
            "2500",
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["command"], "logger-service")
        self.assertEqual(data["duration_ms"], 2500)

    def test_read_snapshot_rejects_duration_override(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            "--bridge-command",
            "read-snapshot",
            "--duration-ms",
            "2500",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("only valid with --bridge-command logger-service", result.stderr)

    def test_config_file_supplies_bench_exe_path(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config_path = Path(td) / "control.json"
            snapshot_path = BUILD_DIR / "fake_logger_service_current_state.json"
            config_path.write_text(
                "{\n"
                f'  "schema_version": 1,\n'
                f'  "bench_exe_path": "{FAKE_BENCH_EXE.as_posix()}",\n'
                '  "poll_ms": 2100,\n'
                f'  "snapshot_path": "{snapshot_path.as_posix()}"\n'
                "}\n",
                encoding="utf-8",
            )

            result = _run_control("--config", str(config_path))

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["command"], "logger-service")
        self.assertEqual(data["duration_ms"], 2100)

    def test_env_bench_path_supplies_bench_exe_path(self) -> None:
        result = _run_control(env={"SVG_MB_CONTROL_BENCH_EXE": str(FAKE_BENCH_EXE)})
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["command"], "logger-service")

    def test_env_config_path_supplies_bench_exe_path(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config_path = Path(td) / "control.json"
            config_path.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                f'  "bench_exe_path": "{FAKE_BENCH_EXE.as_posix()}"\n'
                "}\n",
                encoding="utf-8",
            )

            result = _run_control(env={"SVG_MB_CONTROL_CONFIG": str(config_path)})

        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["command"], "logger-service")

    def test_config_snapshot_path_mismatch_fails_for_logger_service(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config_path = Path(td) / "control.json"
            wrong_snapshot_path = Path(td) / "wrong_current_state.json"
            config_path.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                f'  "bench_exe_path": "{FAKE_BENCH_EXE.as_posix()}",\n'
                '  "poll_ms": 1000,\n'
                f'  "snapshot_path": "{wrong_snapshot_path.as_posix()}"\n'
                "}\n",
                encoding="utf-8",
            )

            result = _run_control("--config", str(config_path))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("snapshot_path does not match", result.stderr)

    def test_explicit_missing_config_fails(self) -> None:
        missing_path = REPO_ROOT / "config" / "does_not_exist.json"
        result = _run_control("--config", str(missing_path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Control config not found", result.stderr)

    def test_real_bench_integration_if_available(self) -> None:
        if not REAL_BENCH_EXE.is_file():
            self.skipTest("real sibling Bench exe is not present")
        if not _is_elevated():
            self.skipTest("real Bench integration requires elevation")

        result = _run_control("--bench-exe-path", str(REAL_BENCH_EXE))
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")

        data = json.loads(result.stdout)
        self.assertIsInstance(data, dict)
        self.assertTrue(data, msg="live snapshot JSON object is empty")

    def test_real_bench_read_snapshot_integration_if_available(self) -> None:
        if not REAL_BENCH_EXE.is_file():
            self.skipTest("real sibling Bench exe is not present")
        if not _is_elevated():
            self.skipTest("real Bench integration requires elevation")

        result = _run_control(
            "--bench-exe-path",
            str(REAL_BENCH_EXE),
            "--bridge-command",
            "read-snapshot",
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")

        data = json.loads(result.stdout)
        self.assertIsInstance(data, dict)
        self.assertTrue(data, msg="live snapshot JSON object is empty")


def _write_read_loop_config(
    td: Path,
    *,
    runtime_home: Path,
    snapshot_path: Path,
    poll_ms: int,
    duration_ms: int,
    staleness_threshold_ms: int | None = None,
    retry_count: int = 3,
    retry_backoff_ms: int = 10,
    child_restart_budget: int = 3,
    child_restart_backoff_ms: int = 100,
) -> Path:
    fields = [
        '  "schema_version": 2',
        f'  "bench_exe_path": "{FAKE_BENCH_EXE.as_posix()}"',
        f'  "snapshot_path": "{snapshot_path.as_posix()}"',
        f'  "poll_ms": {poll_ms}',
        f'  "runtime_home_path": "{runtime_home.as_posix()}"',
        f'  "snapshot_read_retry_count": {retry_count}',
        f'  "snapshot_read_retry_backoff_ms": {retry_backoff_ms}',
        f'  "child_restart_budget": {child_restart_budget}',
        f'  "child_restart_backoff_ms": {child_restart_backoff_ms}',
        f'  "logger_service_duration_ms": {duration_ms}',
    ]
    if staleness_threshold_ms is not None:
        fields.append(f'  "staleness_threshold_ms": {staleness_threshold_ms}')
    config_path = td / "control.json"
    config_path.write_text("{\n" + ",\n".join(fields) + "\n}\n", encoding="utf-8")
    return config_path


def _spawn_read_loop(
    config_path: Path,
    *,
    env_additions: dict[str, str] | None = None,
) -> subprocess.Popen[str]:
    env = os.environ.copy()
    if env_additions:
        env.update(env_additions)
    return subprocess.Popen(
        [str(CONTROL_EXE), "--mode", "read-loop", "--config", str(config_path)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )


def _read_runtime_status(runtime_home: Path) -> dict | None:
    status_path = runtime_home / "control_runtime.json"
    if not status_path.is_file():
        return None
    try:
        return json.loads(status_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def _wait_for(predicate, timeout_s: float, poll_s: float = 0.05):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(poll_s)
    return predicate()


class ReadLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def setUp(self) -> None:
        if FAKE_SNAPSHOT.is_file():
            FAKE_SNAPSHOT.unlink()

    def _stop_and_wait(
        self,
        proc: subprocess.Popen[str],
        *,
        graceful_timeout_s: float = 3.0,
    ) -> tuple[int, str, str]:
        try:
            proc.send_signal(signal.CTRL_BREAK_EVENT)
        except Exception:
            pass
        try:
            stdout, stderr = proc.communicate(timeout=graceful_timeout_s)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
        return proc.returncode, stdout or "", stderr or ""

    def test_read_loop_runtime_home_created(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=150,
                duration_ms=3000,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "emit_snapshots",
                    "SVG_MB_CONTROL_FAKE_PUBLISH_INTERVAL_MS": "100",
                },
            )
            try:
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home), timeout_s=3.0
                )
                self.assertIsNotNone(status, msg="runtime status file never appeared")
                self.assertTrue(runtime_home.is_dir())
                self.assertEqual(status["schema_version"], 1)
            finally:
                self._stop_and_wait(proc)

    def test_read_loop_refreshes_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=150,
                duration_ms=5000,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "emit_snapshots",
                    "SVG_MB_CONTROL_FAKE_PUBLISH_INTERVAL_MS": "100",
                },
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "successful_polls", 0
                    )
                    >= 2,
                    timeout_s=4.0,
                )
                self.assertTrue(
                    observed,
                    msg=f"never saw two successful polls; status={_read_runtime_status(runtime_home)}",
                )
                status = _read_runtime_status(runtime_home)
                self.assertEqual(status["status"], "running")
                self.assertFalse(status["stale"])
                self.assertTrue(status["last_refresh"])
            finally:
                self._stop_and_wait(proc)

    def test_read_loop_restarts_child_on_crash(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=100,
                duration_ms=10000,
                child_restart_budget=5,
                child_restart_backoff_ms=100,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "crash_after_ms",
                    "SVG_MB_CONTROL_FAKE_CRASH_AFTER_MS": "200",
                },
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "restart_count", 0
                    )
                    >= 1,
                    timeout_s=4.0,
                )
                self.assertTrue(
                    observed,
                    msg=f"child never restarted; status={_read_runtime_status(runtime_home)}",
                )
            finally:
                self._stop_and_wait(proc)

    def test_read_loop_exits_on_ctrl_c(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=150,
                duration_ms=20000,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "emit_snapshots",
                    "SVG_MB_CONTROL_FAKE_PUBLISH_INTERVAL_MS": "100",
                },
            )
            _wait_for(lambda: _read_runtime_status(runtime_home), timeout_s=3.0)
            returncode, _stdout, _stderr = self._stop_and_wait(proc)
            self.assertEqual(returncode, 0)
            final_status = _read_runtime_status(runtime_home)
            self.assertIsNotNone(final_status)
            self.assertEqual(final_status["status"], "shutdown")

    def test_read_loop_staleness_detection(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=100,
                duration_ms=20000,
                staleness_threshold_ms=300,
                child_restart_budget=0,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "idle_after_emit",
                },
            )
            try:
                # Child writes one snapshot, then idles for the configured
                # logger_service_duration_ms. Control sees mtime stop
                # advancing, and once staleness_threshold_ms elapses from the
                # last successful poll, the stale flag must flip to true.
                observed_stale = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get("stale")
                    is True,
                    timeout_s=4.0,
                )
                self.assertTrue(
                    observed_stale,
                    msg=f"staleness flag never set; status={_read_runtime_status(runtime_home)}",
                )
            finally:
                self._stop_and_wait(proc)

    def test_read_loop_tolerates_torn_writes(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=FAKE_SNAPSHOT,
                poll_ms=100,
                duration_ms=5000,
                retry_count=0,
                retry_backoff_ms=5,
            )
            proc = _spawn_read_loop(
                config_path,
                env_additions={
                    "SVG_MB_CONTROL_FAKE_MODE": "torn_writes",
                    "SVG_MB_CONTROL_FAKE_PUBLISH_INTERVAL_MS": "100",
                    "SVG_MB_CONTROL_FAKE_TORN_HOLD_MS": "50",
                },
            )
            try:
                observed = _wait_for(
                    lambda: (
                        (_read_runtime_status(runtime_home) or {}).get(
                            "successful_polls", 0
                        )
                        >= 1
                        and (_read_runtime_status(runtime_home) or {}).get(
                            "skipped_polls", 0
                        )
                        >= 1
                    ),
                    timeout_s=5.0,
                )
                self.assertTrue(
                    observed,
                    msg=f"torn writes not exercised; status={_read_runtime_status(runtime_home)}",
                )
            finally:
                self._stop_and_wait(proc)


def _write_phase2_config(
    td: Path,
    *,
    runtime_home: Path,
    baseline_freshness_ceiling_ms: int = 10000,
    restore_timeout_ms: int = 5000,
) -> Path:
    cfg = {
        "schema_version": 3,
        "bench_exe_path": FAKE_BENCH_EXE.as_posix(),
        "runtime_home_path": runtime_home.as_posix(),
        "baseline_freshness_ceiling_ms": baseline_freshness_ceiling_ms,
        "restore_timeout_ms": restore_timeout_ms,
    }
    config_path = td / "control.json"
    config_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    return config_path


def _read_pending_writes(runtime_home: Path) -> list[dict]:
    path = runtime_home / "pending_writes.json"
    if not path.is_file():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("entries", [])


def _seed_pending_writes(runtime_home: Path, entries: list[dict]) -> None:
    runtime_home.mkdir(parents=True, exist_ok=True)
    path = runtime_home / "pending_writes.json"
    payload = {"schema_version": 1, "entries": entries}
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


class WriteOnceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_write_once_happy_path(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {"SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0"}
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"stdout={result.stdout}\nstderr={result.stderr}",
            )
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_baseline_stale_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(
                td,
                runtime_home=runtime_home,
                baseline_freshness_ceiling_ms=500,
            )
            env = {
                "SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0",
                "SVG_MB_CONTROL_FAKE_SNAPSHOT_OFFSET_MS": "5000",
            }
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("snapshot age", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_refused_on_policy_blocked(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {
                "SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0",
                "SVG_MB_CONTROL_FAKE_EFFECTIVE_WRITE_ALLOWED": "false",
                "SVG_MB_CONTROL_FAKE_POLICY_BLOCKED": "true",
            }
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("effective_write_allowed=false", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_refused_on_writes_disabled(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {
                "SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0",
                "SVG_MB_CONTROL_FAKE_POLICY_WRITES_ENABLED": "false",
            }
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("writes_enabled=false", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_child_exit_2_clears_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            # Snapshot has policy_writes_enabled=true so Control passes the
            # pre-spawn check and writes the sidecar. The fake child then
            # exits with code 2 (policy refusal), which Control treats as
            # "no fan touched" and clears the sidecar.
            env = {
                "SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0",
                "SVG_MB_CONTROL_FAKE_POLICY_WRITES_ENABLED": "true",
                "SVG_MB_CONTROL_FAKE_WRITE_MODE": "policy_refused",
            }
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_child_nonzero_keeps_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {
                "SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0",
                "SVG_MB_CONTROL_FAKE_WRITE_MODE": "fail_immediate",
            }
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "0",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            entries = _read_pending_writes(runtime_home)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["channel"], 0)

    def test_write_once_channel_not_in_snapshot_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {"SVG_MB_CONTROL_FAKE_FAN_CHANNEL": "0"}
            result = _run_control(
                "--mode",
                "write-once",
                "--config",
                str(config_path),
                "--write-channel",
                "3",
                "--write-pct",
                "50",
                "--write-hold-ms",
                "200",
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not present in snapshot", result.stderr)

    def test_reconcile_runs_restore_on_startup(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 4,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "bench_started_iso": "2026-04-05T01:00:00",
                        "bench_child_pid": 0,
                    }
                ],
            )
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            # Use one-shot mode; reconciliation runs first regardless of mode.
            result = _run_control(
                "--bench-exe-path",
                str(FAKE_BENCH_EXE),
                "--config",
                str(config_path),
            )
            self.assertEqual(
                result.returncode,
                0,
                msg=f"stdout={result.stdout}\nstderr={result.stderr}",
            )
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_reconcile_failure_blocks_startup(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 4,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "bench_started_iso": "2026-04-05T01:00:00",
                        "bench_child_pid": 0,
                    }
                ],
            )
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = {"SVG_MB_CONTROL_FAKE_RESTORE_MODE": "fail"}
            result = _run_control(
                "--bench-exe-path",
                str(FAKE_BENCH_EXE),
                "--config",
                str(config_path),
                env=env,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reconciliation failed", result.stderr)
            entries = _read_pending_writes(runtime_home)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["channel"], 4)

    def test_write_once_ctrl_break_terminates_child(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)
            env = os.environ.copy()
            env["SVG_MB_CONTROL_FAKE_FAN_CHANNEL"] = "0"
            proc = subprocess.Popen(
                [
                    str(CONTROL_EXE),
                    "--mode",
                    "write-once",
                    "--config",
                    str(config_path),
                    "--write-channel",
                    "0",
                    "--write-pct",
                    "50",
                    "--write-hold-ms",
                    "30000",
                ],
                cwd=REPO_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
            )
            # Give Control a moment to spawn the child.
            time.sleep(0.5)
            proc.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                stdout, stderr = proc.communicate(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
            self.assertEqual(
                proc.returncode,
                0,
                msg=f"Control did not exit cleanly on CTRL_BREAK; stderr={stderr}",
            )
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_two_concurrent_control_instances_share_sidecar(self) -> None:
        """Observational test: two Control processes against the same runtime
        home writing different channels. Records the resulting sidecar state
        and exit codes rather than enforcing a specific ordering."""
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_phase2_config(td, runtime_home=runtime_home)

            def spawn(channel: int) -> subprocess.Popen:
                env = os.environ.copy()
                env["SVG_MB_CONTROL_FAKE_FAN_CHANNEL"] = str(channel)
                return subprocess.Popen(
                    [
                        str(CONTROL_EXE),
                        "--mode",
                        "write-once",
                        "--config",
                        str(config_path),
                        "--write-channel",
                        str(channel),
                        "--write-pct",
                        "50",
                        "--write-hold-ms",
                        "500",
                    ],
                    cwd=REPO_ROOT,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=env,
                )

            # Note: fake-bench emits its snapshot with a single channel set
            # via env var, but each Control reads its own snapshot, so both
            # will see the expected fan for their target channel. This does
            # not perfectly model real hardware.
            proc_a = spawn(0)
            proc_b = spawn(1)
            out_a, err_a = proc_a.communicate(timeout=15.0)
            out_b, err_b = proc_b.communicate(timeout=15.0)
            final_entries = _read_pending_writes(runtime_home)
            # Loose assertions: the test documents observed behavior.
            self.assertIn(
                proc_a.returncode,
                [0, 1, 3, 4, 5],
                msg=f"unexpected exit {proc_a.returncode}; stderr={err_a}",
            )
            self.assertIn(
                proc_b.returncode,
                [0, 1, 3, 4, 5],
                msg=f"unexpected exit {proc_b.returncode}; stderr={err_b}",
            )
            # Not asserting final_entries length — this test records behavior.
            # A cleanup restore may be needed in real operation.
            if final_entries:
                print(
                    "\n[observation] two concurrent Control instances left "
                    f"{len(final_entries)} pending entries in the sidecar"
                )
