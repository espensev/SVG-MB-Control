from __future__ import annotations

import csv
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
_TEST_EXE_ENV = os.environ.get("SVG_MB_CONTROL_TEST_EXE")
CONTROL_EXE = Path(_TEST_EXE_ENV) if _TEST_EXE_ENV else BUILD_DIR / "svg-mb-control.exe"
_TEST_TASK_RUNNER_ENV = os.environ.get("SVG_MB_CONTROL_TEST_TASK_RUNNER_EXE")
CONTROL_TASK_RUNNER_EXE = (
    Path(_TEST_TASK_RUNNER_ENV)
    if _TEST_TASK_RUNNER_ENV
    else BUILD_DIR / "svg-mb-control-task-runner.exe"
)
BUILD_SCRIPT = REPO_ROOT / "build-release.ps1"


def _ensure_release_build() -> None:
    if CONTROL_EXE.is_file() and CONTROL_TASK_RUNNER_EXE.is_file():
        return
    if _TEST_EXE_ENV or _TEST_TASK_RUNNER_ENV:
        missing = [
            str(path)
            for path in (CONTROL_EXE, CONTROL_TASK_RUNNER_EXE)
            if not path.is_file()
        ]
        raise unittest.SkipTest(
            "configured test executable not found: " + ", ".join(missing)
        )

    result = subprocess.run(
        [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(BUILD_SCRIPT),
            "-NoStopProcesses",
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


class RuntimeProbe:
    def __init__(
        self,
        args: list[str],
        *,
        env: dict[str, str] | None = None,
        cwd: Path = REPO_ROOT,
        exe: Path = CONTROL_EXE,
        graceful_timeout_s: float = 4.0,
    ) -> None:
        self.args = args
        self.env = env
        self.cwd = cwd
        self.exe = exe
        self.graceful_timeout_s = graceful_timeout_s
        self.proc: subprocess.Popen[str] | None = None
        self.stop_result: tuple[int, str, str] | None = None

    def __enter__(self) -> RuntimeProbe:
        self.proc = _spawn_control(
            self.args,
            env=self.env,
            cwd=self.cwd,
            exe=self.exe,
        )
        return self

    @property
    def process(self) -> subprocess.Popen[str]:
        if self.proc is None:
            raise RuntimeError("runtime probe has not been started")
        return self.proc

    def stop(self) -> tuple[int, str, str]:
        if self.stop_result is None:
            self.stop_result = _stop_and_wait(
                self.process,
                graceful_timeout_s=self.graceful_timeout_s,
            )
        return self.stop_result

    def __exit__(self, exc_type, exc, tb) -> bool:
        if self.proc is not None:
            self.stop()
        return False


class StagedControlApp:
    def __init__(
        self,
        *,
        default_mode: str = "read-loop",
        poll_ms: int = 100,
    ) -> None:
        self.default_mode = default_mode
        self.poll_ms = poll_ms
        self._tempdir: tempfile.TemporaryDirectory[str] | None = None
        self.root: Path | None = None
        self.exe: Path | None = None
        self.runtime_home: Path | None = None
        self.start_result: subprocess.CompletedProcess[str] | None = None
        self.stop_result: subprocess.CompletedProcess[str] | None = None
        self._started = False

    def __enter__(self) -> StagedControlApp:
        self._tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self._tempdir.name)
        self.exe = self.root / "svg-mb-control.exe"
        self.runtime_home = self.root / "runtime"
        shutil.copy2(CONTROL_EXE, self.exe)
        _write_read_loop_config(
            self.root,
            runtime_home=self.runtime_home,
            default_mode=self.default_mode,
            poll_ms=self.poll_ms,
        )
        return self

    def start(
        self,
        *,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        result = self.run(env=env)
        self.start_result = result
        self._started = result.returncode == 0
        return result

    def run(
        self,
        *args: str,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return _run_control(
            *args,
            cwd=self._root,
            exe=self._exe,
            env=env,
        )

    def status(self) -> subprocess.CompletedProcess[str]:
        return self.run("--status")

    def restart(
        self,
        *,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        result = self.run("--restart", env=env)
        if result.returncode == 0:
            self._started = True
            self.stop_result = None
        return result

    def stop(self) -> subprocess.CompletedProcess[str]:
        process_ids = self._published_process_ids()
        if self.stop_result is None:
            self.stop_result = self.run("--stop")
        self._wait_for_staged_processes_to_exit(process_ids)
        self._started = False
        return self.stop_result

    @property
    def _root(self) -> Path:
        if self.root is None:
            raise RuntimeError("staged control app has not been entered")
        return self.root

    @property
    def _exe(self) -> Path:
        if self.exe is None:
            raise RuntimeError("staged control app has not been entered")
        return self.exe

    @property
    def _runtime_home_path(self) -> Path:
        if self.runtime_home is None:
            raise RuntimeError("staged control app has not been entered")
        return self.runtime_home

    def _published_process_ids(self) -> set[int]:
        process_ids: set[int] = set()
        status = _read_runtime_status(self._runtime_home_path)
        if status is not None:
            self._add_process_id(process_ids, status.get("process_id"))

        supervisor = _read_json(self._runtime_home_path / "control_supervisor.json")
        if supervisor is not None:
            self._add_process_id(process_ids, supervisor.get("supervisor_pid"))
            self._add_process_id(process_ids, supervisor.get("last_worker_pid"))
        return process_ids

    @staticmethod
    def _add_process_id(process_ids: set[int], value: object) -> None:
        try:
            pid = int(value)
        except (TypeError, ValueError):
            return
        if pid > 0:
            process_ids.add(pid)

    def _wait_for_staged_processes_to_exit(
        self,
        process_ids: set[int],
        *,
        timeout_s: float = 5.0,
    ) -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            process_ids.update(self._published_process_ids())
            if process_ids and not self._svg_processes_are_running(process_ids):
                return
            status = _read_runtime_status(self._runtime_home_path)
            if not process_ids and status and status.get("status") == "shutdown":
                return
            time.sleep(0.05)

        process_ids.update(self._published_process_ids())
        self._terminate_svg_processes(process_ids)

    @staticmethod
    def _svg_processes_are_running(process_ids: set[int]) -> bool:
        if not process_ids:
            return False
        return StagedControlApp._run_process_filter(
            process_ids,
            "exit @(Get-Process svg-mb-control -ErrorAction SilentlyContinue | "
            "Where-Object { $ids -contains $_.Id }).Count",
        ).returncode != 0

    def _terminate_svg_processes(self, process_ids: set[int]) -> None:
        if not process_ids:
            return
        # Defense-in-depth on top of PID scoping: only stop a PID-matched
        # process whose image path is under this test's staged root, so a
        # recycled PID that now belongs to a live release\ instance is never
        # killed. The staged app runs a copy of the exe from self.root (see
        # __enter__), so its .Path is under that root; a packaged controller
        # running from release\ is not.
        self._run_process_filter(
            process_ids,
            "Get-Process svg-mb-control -ErrorAction SilentlyContinue | "
            "Where-Object { $ids -contains $_.Id } | "
            "Where-Object { $_.Path -and "
            "$_.Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase) } | "
            "Stop-Process -Force -ErrorAction SilentlyContinue",
            root=self._root,
        )

    @staticmethod
    def _run_process_filter(
        process_ids: set[int],
        command: str,
        root: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        ids = ",".join(str(pid) for pid in sorted(process_ids))
        setup = f"$ids=@({ids});"
        if root is not None:
            root_str = str(root)
            if not root_str.endswith(("\\", "/")):
                root_str += "\\"
            setup += " $root='" + root_str.replace("'", "''") + "';"
        return subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                f"{setup} {command}",
            ],
            capture_output=True,
            text=True,
        )

    def __exit__(self, exc_type, exc, tb) -> bool:
        try:
            if self._started:
                self.stop()
        finally:
            if self._tempdir is not None:
                self._tempdir.cleanup()
        return False


def _read_json(path: Path) -> dict | None:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except PermissionError:
        return None
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


def _read_runtime_events(runtime_home: Path) -> list[dict]:
    path = runtime_home / "logs" / "svg_mb_control_events.jsonl"
    return _read_jsonl(path)


def _read_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    events: list[dict] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        events.append(json.loads(line))
    return events


def _read_runtime_manifest(runtime_home: Path) -> dict | None:
    return _read_json(runtime_home / "logs" / "svg_mb_control_manifest.json")


def _runtime_archive_files(runtime_home: Path) -> list[Path]:
    archive_dir = runtime_home / "logs" / "archive"
    if not archive_dir.is_dir():
        return []
    return sorted(archive_dir.glob("*.csv"))


def _read_runtime_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    lines = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    if len(lines) < 2:
        return []
    return list(csv.DictReader(lines))


def _read_runtime_csv_prologue(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#"):
            break
        body = line[1:].strip()
        key, sep, value = body.partition("=")
        if sep:
            fields[key] = value
    return fields


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
    evidence_gpu_sample_mode: str | None = None,
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
    if evidence_gpu_sample_mode is not None:
        cfg["evidence_gpu_sample_mode"] = evidence_gpu_sample_mode
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
    restore_timeout_ms: int = 5000,
    poll_tick_ms: int = 200,
    write_cooldown_ms: int = 500,
    deadband_pct: float = 3.0,
    control_hold_ms: int = 1000,
    curve: list[tuple[float, float]] | None = None,
    cpu_override_curve: list[tuple[float, float]] | None = None,
    min_duty_pct: float = 40.0,
    temp_blend: str = "cpu_only",
    channel_write_cooldown_ms: int | None = None,
    channel_deadband_pct: float | None = None,
    channel_control_hold_ms: int | None = None,
    extra_loop_fields: dict[str, object] | None = None,
    extra_channel_fields: dict[str, object] | None = None,
) -> Path:
    if curve is None:
        curve = [(30.0, 40.0), (60.0, 55.0), (80.0, 80.0), (95.0, 100.0)]

    channel_cfg: dict[str, object] = {
        "channel": channel,
        "temp_blend": temp_blend,
        "min_duty_pct": min_duty_pct,
        "curve": [{"temp_c": t, "duty_pct": d} for (t, d) in curve],
    }
    if cpu_override_curve is not None:
        channel_cfg["cpu_override_curve"] = [
            {"temp_c": t, "duty_pct": d} for (t, d) in cpu_override_curve
        ]
    if channel_write_cooldown_ms is not None:
        channel_cfg["write_cooldown_ms"] = channel_write_cooldown_ms
    if channel_deadband_pct is not None:
        channel_cfg["deadband_pct"] = channel_deadband_pct
    if channel_control_hold_ms is not None:
        channel_cfg["control_hold_ms"] = channel_control_hold_ms
    if extra_channel_fields is not None:
        channel_cfg.update(extra_channel_fields)

    cfg: dict[str, object] = {
        "schema_version": 4,
        "runtime_home_path": runtime_home.as_posix(),
        "poll_ms": 100,
        "baseline_freshness_ceiling_ms": 10000,
        "restore_timeout_ms": restore_timeout_ms,
        "control_loop": {
            "poll_tick_ms": poll_tick_ms,
            "write_cooldown_ms": write_cooldown_ms,
            "deadband_pct": deadband_pct,
            "control_hold_ms": control_hold_ms,
            "cpu_temp_label": "Tctl/Tdie",
            "channels": [channel_cfg],
        },
    }
    if extra_loop_fields is not None:
        cfg["control_loop"].update(extra_loop_fields)
    if runtime_policy_path is not None:
        cfg["runtime_policy_path"] = runtime_policy_path.as_posix()
    return _write_json(td / "control.json", cfg)

__all__ = [name for name in globals() if not name.startswith('__')]
