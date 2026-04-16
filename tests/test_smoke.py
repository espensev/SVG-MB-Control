from __future__ import annotations

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
BUILD_SCRIPT = REPO_ROOT / "build-release.ps1"


def _ensure_release_build() -> None:
    if CONTROL_EXE.is_file():
        return

    result = subprocess.run(
        [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(BUILD_SCRIPT),
            "-SkipTests",
            "-KeepBuildDir",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise unittest.SkipTest(
            "release build failed:\n"
            f"{result.stdout}\n{result.stderr}"
        )


def _merged_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE", "disabled")
    if extra:
        env.update(extra)
    return env


def _sim_direct_env(
    *,
    channel: int = 0,
    amd_temp_c: float = 75.0,
    duty_raw: int = 128,
    mode_raw: int = 5,
    read_channel: int | None = None,
    read_duty_raw: int | None = None,
    read_mode_raw: int | None = None,
) -> dict[str, str]:
    env = {
        "SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE": "enabled",
        "SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE": "enabled",
        "SVG_MB_CONTROL_SIM_AMD_TCTL_C": str(amd_temp_c),
        "SVG_MB_CONTROL_SIM_FAN_CHANNEL": str(channel),
        "SVG_MB_CONTROL_SIM_FAN_DUTY_RAW": str(duty_raw),
        "SVG_MB_CONTROL_SIM_FAN_MODE_RAW": str(mode_raw),
    }
    if read_channel is not None:
        env["SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL"] = str(read_channel)
    if read_duty_raw is not None:
        env["SVG_MB_CONTROL_SIM_READ_FAN_DUTY_RAW"] = str(read_duty_raw)
    if read_mode_raw is not None:
        env["SVG_MB_CONTROL_SIM_READ_FAN_MODE_RAW"] = str(read_mode_raw)
    return env


def _run_control(
    *args: str,
    env: dict[str, str] | None = None,
    cwd: Path = REPO_ROOT,
    exe: Path = CONTROL_EXE,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(exe), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        env=_merged_env(env),
    )


def _spawn_control(
    args: list[str],
    *,
    env: dict[str, str] | None = None,
    cwd: Path = REPO_ROOT,
    exe: Path = CONTROL_EXE,
) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [str(exe), *args],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=_merged_env(env),
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )


def _wait_for(predicate, timeout_s: float, poll_s: float = 0.05):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(poll_s)
    return predicate()


def _stop_and_wait(
    proc: subprocess.Popen[str], *, graceful_timeout_s: float = 4.0
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


def _read_json(path: Path) -> dict | None:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def _read_runtime_status(runtime_home: Path) -> dict | None:
    return _read_json(runtime_home / "control_runtime.json")


def _read_runtime_current_state(runtime_home: Path) -> dict | None:
    return _read_json(runtime_home / "current_state.json")


def _read_pending_writes(runtime_home: Path) -> list[dict]:
    data = _read_json(runtime_home / "pending_writes.json")
    if not data:
        return []
    return data.get("entries", [])


def _seed_pending_writes(runtime_home: Path, entries: list[dict]) -> None:
    runtime_home.mkdir(parents=True, exist_ok=True)
    payload = {"schema_version": 1, "entries": entries}
    (runtime_home / "pending_writes.json").write_text(
        json.dumps(payload, indent=2),
        encoding="utf-8",
    )


def _write_json(path: Path, payload: dict) -> Path:
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return path


def _write_read_loop_config(
    td: Path,
    *,
    runtime_home: Path,
    snapshot_path: Path | None = None,
    default_mode: str | None = None,
    poll_ms: int = 100,
    staleness_threshold_ms: int | None = None,
) -> Path:
    cfg: dict[str, object] = {
        "schema_version": 4,
        "runtime_home_path": runtime_home.as_posix(),
        "poll_ms": poll_ms,
        "baseline_freshness_ceiling_ms": 10000,
        "restore_timeout_ms": 5000,
    }
    if default_mode is not None:
        cfg["default_mode"] = default_mode
    if snapshot_path is not None:
        cfg["snapshot_path"] = snapshot_path.as_posix()
    if staleness_threshold_ms is not None:
        cfg["staleness_threshold_ms"] = staleness_threshold_ms
    return _write_json(td / "control.json", cfg)


def _write_write_once_config(
    td: Path,
    *,
    runtime_home: Path,
    baseline_freshness_ceiling_ms: int = 10000,
    restore_timeout_ms: int = 5000,
    write_channel: int | None = None,
    write_target_pct: float | None = None,
    write_hold_ms: int | None = None,
) -> Path:
    cfg: dict[str, object] = {
        "schema_version": 4,
        "runtime_home_path": runtime_home.as_posix(),
        "baseline_freshness_ceiling_ms": baseline_freshness_ceiling_ms,
        "restore_timeout_ms": restore_timeout_ms,
    }
    if write_channel is not None:
        cfg["write_channel"] = write_channel
    if write_target_pct is not None:
        cfg["write_target_pct"] = write_target_pct
    if write_hold_ms is not None:
        cfg["write_hold_ms"] = write_hold_ms
    return _write_json(td / "control.json", cfg)


def _write_control_loop_config(
    td: Path,
    *,
    runtime_home: Path,
    channel: int,
    runtime_policy_path: Path | None = None,
    poll_tick_ms: int = 200,
    write_cooldown_ms: int = 500,
    deadband_pct: float = 3.0,
    control_hold_ms: int = 1000,
    curve: list[tuple[float, float]] | None = None,
    min_duty_pct: float = 40.0,
    temp_blend: str = "cpu_only",
    channel_write_cooldown_ms: int | None = None,
    channel_deadband_pct: float | None = None,
    channel_control_hold_ms: int | None = None,
) -> Path:
    if curve is None:
        curve = [(30.0, 40.0), (60.0, 55.0), (80.0, 80.0), (95.0, 100.0)]

    channel_cfg: dict[str, object] = {
        "channel": channel,
        "temp_blend": temp_blend,
        "min_duty_pct": min_duty_pct,
        "curve": [{"temp_c": t, "duty_pct": d} for (t, d) in curve],
    }
    if channel_write_cooldown_ms is not None:
        channel_cfg["write_cooldown_ms"] = channel_write_cooldown_ms
    if channel_deadband_pct is not None:
        channel_cfg["deadband_pct"] = channel_deadband_pct
    if channel_control_hold_ms is not None:
        channel_cfg["control_hold_ms"] = channel_control_hold_ms

    cfg: dict[str, object] = {
        "schema_version": 4,
        "runtime_home_path": runtime_home.as_posix(),
        "poll_ms": 100,
        "baseline_freshness_ceiling_ms": 10000,
        "restore_timeout_ms": 5000,
        "control_loop": {
            "poll_tick_ms": poll_tick_ms,
            "write_cooldown_ms": write_cooldown_ms,
            "deadband_pct": deadband_pct,
            "control_hold_ms": control_hold_ms,
            "cpu_temp_label": "Tctl/Tdie",
            "channels": [channel_cfg],
        },
    }
    if runtime_policy_path is not None:
        cfg["runtime_policy_path"] = runtime_policy_path.as_posix()
    return _write_json(td / "control.json", cfg)


class SmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_direct_one_shot_outputs_json(self) -> None:
        result = _run_control(
            "--mode",
            "one-shot",
            env=_sim_direct_env(channel=0, amd_temp_c=81.5, duty_raw=128, mode_raw=5),
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        data = json.loads(result.stdout)
        self.assertEqual(data["amd_sensors"][0]["label"], "Tctl/Tdie")
        self.assertEqual(data["amd_sensors"][0]["temperature_c"], 81.5)
        self.assertEqual(data["fans"][0]["channel"], 0)
        self.assertEqual(data["fans"][0]["duty_raw"], 128)
        self.assertEqual(data["fans"][0]["mode_raw"], 5)

    def test_diagnose_amd_sim_mode_outputs_snapshot(self) -> None:
        result = _run_control(
            "--diagnose-amd",
            env={
                "SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE": "enabled",
                "SVG_MB_CONTROL_SIM_AMD_TCTL_C": "82.5",
            },
        )
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
        self.assertIn("amd_reader.available: true", result.stdout)
        self.assertIn('sample.available: true', result.stdout)
        self.assertIn('sample[0].label: "Tctl/Tdie"', result.stdout)
        self.assertIn("sample[0].temperature_c: 82.5", result.stdout)

    def test_removed_bridge_flags_fail_clearly(self) -> None:
        result = _run_control("--bridge-exe-path", "legacy-bridge.exe")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Legacy bridge options were removed", result.stderr)

    def test_removed_bridge_config_fields_fail_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "default_mode": "one-shot",
                    "bridge_exe_path": "legacy-bridge.exe",
                },
            )
            result = _run_control("--config", str(config_path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Legacy control config field was removed: bridge_exe_path",
                result.stderr,
            )

    def test_removed_bench_runtime_policy_alias_fails_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "default_mode": "one-shot",
                    "bench_runtime_policy_path": "runtime_policy_write_live.json",
                },
            )
            result = _run_control("--config", str(config_path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Legacy control config field was removed: bench_runtime_policy_path",
                result.stderr,
            )

    def test_zero_arg_staged_launch_uses_control_json_default_mode(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            staged_exe = td / "svg-mb-control.exe"
            runtime_home = td / "runtime"
            shutil.copy2(CONTROL_EXE, staged_exe)
            _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                default_mode="read-loop",
                poll_ms=100,
            )

            proc = _spawn_control(
                [],
                cwd=td,
                exe=staged_exe,
                env=_sim_direct_env(channel=1, amd_temp_c=74.0),
            )
            try:
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status, msg="control_runtime.json never appeared")
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                self.assertEqual(state["fans"][0]["channel"], 1)
                self.assertEqual(state["amd_sensors"][0]["temperature_c"], 74.0)
            finally:
                _stop_and_wait(proc)


class ReadLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_read_loop_publishes_control_owned_current_state_and_mirror(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            snapshot_path = td / "mirror" / "current_state.json"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=snapshot_path,
                poll_ms=100,
            )
            proc = _spawn_control(
                ["--mode", "read-loop", "--config", str(config_path)],
                env=_sim_direct_env(
                    channel=0,
                    amd_temp_c=83.0,
                    duty_raw=90,
                    mode_raw=7,
                    read_channel=0,
                    read_duty_raw=222,
                    read_mode_raw=7,
                ),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status, msg="control_runtime.json never appeared")
                mirror = _wait_for(
                    lambda: _read_json(snapshot_path),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(mirror, msg="snapshot mirror never appeared")

                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertEqual(fan0["duty_raw"], 222)
                self.assertEqual(fan0["mode_raw"], 7)
                self.assertEqual(state["amd_sensors"][0]["temperature_c"], 83.0)
                self.assertEqual(mirror["fans"][0]["duty_raw"], 222)
                self.assertGreaterEqual(status["successful_polls"], 1)
                self.assertEqual(status["restart_count"], 0)
                self.assertEqual(status["child_pid"], 0)
            finally:
                _stop_and_wait(proc)

    def test_read_loop_shutdown_updates_runtime_status(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                poll_ms=100,
            )
            proc = _spawn_control(
                ["--mode", "read-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=70.0),
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "successful_polls", 0
                    )
                    >= 1,
                    timeout_s=5.0,
                )
                self.assertTrue(observed)
            finally:
                code, _, stderr = _stop_and_wait(proc)

            self.assertEqual(code, 0, msg=stderr)
            final_status = _read_runtime_status(runtime_home)
            self.assertIsNotNone(final_status)
            self.assertEqual(final_status["status"], "shutdown")
            self.assertEqual(final_status["status_detail"], "stop requested")


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
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
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
                env=_sim_direct_env(channel=0, amd_temp_c=76.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_uses_env_config_path_and_config_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                write_channel=0,
                write_target_pct=45.0,
                write_hold_ms=150,
            )
            result = _run_control(
                "--mode",
                "write-once",
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_CONFIG": str(config_path),
                },
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_baseline_stale_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                baseline_freshness_ceiling_ms=500,
            )
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
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_SNAPSHOT_OFFSET_MS": "5000",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("snapshot age", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

    def test_write_once_refused_on_policy_blocked(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
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
                env={
                    **_sim_direct_env(channel=0, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_EFFECTIVE_WRITE_ALLOWED": "false",
                    "SVG_MB_CONTROL_SIM_POLICY_BLOCKED": "true",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("effective_write_allowed=false", result.stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])

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
                        "started_iso": "2026-04-05T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--config",
                str(config_path),
                env=_sim_direct_env(channel=4, amd_temp_c=76.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
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
                        "started_iso": "2026-04-05T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            result = _run_control(
                "--config",
                str(config_path),
                env={
                    **_sim_direct_env(channel=4, amd_temp_c=76.0),
                    "SVG_MB_CONTROL_SIM_RESTORE_MODE": "fail",
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reconciliation failed", result.stderr)
            self.assertEqual(len(_read_pending_writes(runtime_home)), 1)

    def test_write_once_ctrl_break_clears_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_write_once_config(td, runtime_home=runtime_home)
            proc = _spawn_control(
                [
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
                env=_sim_direct_env(channel=0, amd_temp_c=76.0),
            )
            try:
                observed = _wait_for(
                    lambda: len(_read_pending_writes(runtime_home)) == 1,
                    timeout_s=5.0,
                )
                self.assertTrue(observed, msg="pending_writes.json was never created")
            finally:
                code, _, stderr = _stop_and_wait(proc)

            self.assertEqual(code, 0, msg=stderr)
            self.assertEqual(_read_pending_writes(runtime_home), [])


class ControlLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_control_loop_ticks_and_writes(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=200,
                write_cooldown_ms=400,
                deadband_pct=2.0,
                control_hold_ms=800,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "loop_tick_count", 0
                    )
                    >= 3,
                    timeout_s=5.0,
                )
                self.assertTrue(observed)
                status = _read_runtime_status(runtime_home)
                self.assertIsNotNone(status)
                self.assertEqual(status["mode"], "control-loop")
                self.assertGreaterEqual(
                    status["controlled_channels"][0]["total_writes"], 1
                )
            finally:
                _stop_and_wait(proc)

    def test_control_loop_current_state_prefers_direct_fan_telemetry(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(
                    channel=0,
                    amd_temp_c=75.0,
                    duty_raw=80,
                    mode_raw=3,
                    read_channel=0,
                    read_duty_raw=222,
                    read_mode_raw=7,
                ),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertEqual(fan0["duty_raw"], 222)
                self.assertEqual(fan0["mode_raw"], 7)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_uses_local_runtime_policy_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            policy_path = td / "runtime_policy.json"
            _write_json(
                policy_path,
                {
                    "schema_version": 1,
                    "profile_name": "blocked",
                    "board": "test",
                    "control": {
                        "writes_enabled": True,
                        "restore_on_exit": True,
                        "blocked_channels": [0],
                    },
                },
            )
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                runtime_policy_path=policy_path,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=82.0),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state)
                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertTrue(fan0["policy_blocked"])
                self.assertFalse(fan0["effective_write_allowed"])

                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status)
                self.assertEqual(status["controlled_channels"][0]["total_writes"], 0)
            finally:
                _stop_and_wait(proc)

    def test_control_loop_empty_channels_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_json(
                td / "control.json",
                {
                    "schema_version": 4,
                    "runtime_home_path": runtime_home.as_posix(),
                    "control_loop": {
                        "poll_tick_ms": 200,
                        "channels": [],
                    },
                },
            )
            result = _run_control(
                "--mode",
                "control-loop",
                "--config",
                str(config_path),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("empty channels array", result.stderr)

    def test_control_loop_channel_hold_override_flows_to_pending_write(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_control_loop_config(
                td,
                runtime_home=runtime_home,
                channel=0,
                poll_tick_ms=100,
                write_cooldown_ms=100,
                deadband_pct=2.0,
                control_hold_ms=800,
                channel_control_hold_ms=2222,
            )
            proc = _spawn_control(
                ["--mode", "control-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=75.0),
            )
            try:
                entry = _wait_for(
                    lambda: next(
                        (
                            item
                            for item in _read_pending_writes(runtime_home)
                            if item.get("channel") == 0
                        ),
                        None,
                    ),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(entry, msg="pending write entry was not created")
                self.assertEqual(entry["requested_hold_ms"], 2222)
            finally:
                _stop_and_wait(proc)


if __name__ == "__main__":
    unittest.main()
