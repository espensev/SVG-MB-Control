from __future__ import annotations

import ctypes
import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build" / "x64-release"
CONTROL_EXE = BUILD_DIR / "svg-mb-control.exe"
FAKE_BENCH_EXE = BUILD_DIR / "fake-bench.exe"
REAL_BENCH_EXE = REPO_ROOT.parent / "SVG-MB-Bench" / "svg-mb-bench.exe"


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

    def test_fake_bench_success_outputs_json(self) -> None:
        result = _run_control("--bench-exe-path", str(FAKE_BENCH_EXE))
        self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")

        data = json.loads(result.stdout)
        self.assertEqual(data["source"], "fake-bench")
        self.assertEqual(data["mode"], "success")
        self.assertEqual(data["sample"], 1)

    def test_fake_bench_missing_snapshot_path_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            env={"SVG_MB_CONTROL_FAKE_MODE": "missing_snapshot_path"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("snapshot_archive", result.stderr)

    def test_fake_bench_nonzero_exit_fails(self) -> None:
        result = _run_control(
            "--bench-exe-path",
            str(FAKE_BENCH_EXE),
            env={"SVG_MB_CONTROL_FAKE_MODE": "fail_exit"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("read-snapshot exited", result.stderr)

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
