from __future__ import annotations

from tests.helpers import *


class RuntimeHealthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def _run_health(self, config_path: Path) -> tuple[int, dict, str]:
        result = _run_control("--health", "--json", "--config", str(config_path))
        self.assertEqual(result.stderr, "")
        self.assertTrue(result.stdout.strip(), msg="health JSON was empty")
        return result.returncode, json.loads(result.stdout), result.stdout

    def test_health_json_reports_stopped_when_status_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 2)
            self.assertEqual(payload["health_state"], "stopped")
            self.assertFalse(payload["status_file_present"])

    def test_health_json_reports_failed_when_status_is_malformed(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            (runtime_home / "control_runtime.json").write_text(
                '{"status": "running"} trailing',
                encoding="utf-8",
            )
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 3)
            self.assertEqual(payload["health_state"], "failed")
            self.assertTrue(payload["status_file_present"])
            self.assertFalse(payload["status_json_valid"])

    def test_health_json_reports_stale_for_old_active_status(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            _write_json(
                runtime_home / "control_runtime.json",
                {
                    "schema_version": 1,
                    "mode": "read-loop",
                    "process_id": os.getpid(),
                    "status": "running",
                    "status_detail": "direct sample refreshed",
                    "last_refresh": "2001-01-01T00:00:00",
                    "stale": False,
                },
            )
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 2)
            self.assertEqual(payload["health_state"], "stale")
            self.assertTrue(payload["process_active"])

    def test_health_json_reports_degraded_for_channel_breaker(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            _write_json(
                runtime_home / "control_runtime.json",
                {
                    "schema_version": 4,
                    "mode": "control-loop",
                    "process_id": os.getpid(),
                    "status": "running",
                    "status_detail": "tick complete",
                    "loop_last_evaluation": time.strftime("%Y-%m-%dT%H:%M:%S"),
                    "controlled_channels": [
                        {
                            "channel": 1,
                            "circuit_breaker_open": True,
                            "sensor_failed": False,
                            "consecutive_write_failures": 0,
                        }
                    ],
                },
            )
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 1)
            self.assertEqual(payload["health_state"], "degraded")
            self.assertEqual(payload["degraded_channel_count"], 1)

    def test_status_json_uses_health_contract(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            result = _run_control("--status", "--json", "--config", str(config_path))

            self.assertEqual(result.returncode, 2)
            payload = json.loads(result.stdout)
            self.assertEqual(payload["health_state"], "stopped")
