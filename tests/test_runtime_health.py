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

    def _write_healthy_status(self, runtime_home: Path, **extra: object) -> None:
        runtime_home.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema_version": 4,
            "mode": "control-loop",
            "process_id": os.getpid(),
            "status": "running",
            "status_detail": "tick complete",
            "loop_last_evaluation": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "controlled_channels": [],
        }
        payload.update(extra)
        _write_json(runtime_home / "control_runtime.json", payload)

    def test_health_json_merges_supervisor_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            self._write_healthy_status(runtime_home)
            _write_json(
                runtime_home / "control_supervisor.json",
                {
                    "schema_version": 1,
                    "supervisor_pid": os.getpid(),
                    "worker_restart_count": 3,
                    "last_worker_pid": 4242,
                    "last_worker_started_time": "2026-05-18T10:00:00",
                    "last_worker_restart_time": "2026-05-18T10:05:00",
                    "last_worker_exit_time": "2026-05-18T10:04:59",
                    "last_worker_exit_code": 1,
                },
            )
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 0)
            self.assertEqual(payload["health_state"], "healthy")
            self.assertTrue(payload["supervisor_state_present"])
            self.assertEqual(payload["supervisor_pid"], os.getpid())
            self.assertTrue(payload["supervisor_active"])
            self.assertEqual(payload["worker_restart_count"], 3)
            self.assertEqual(payload["last_worker_pid"], 4242)
            self.assertEqual(payload["last_worker_exit_code"], 1)
            self.assertEqual(
                payload["last_worker_restart_time"], "2026-05-18T10:05:00"
            )

    def test_health_json_absent_supervisor_sidecar_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            self._write_healthy_status(runtime_home)
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            self.assertEqual(exit_code, 0)
            self.assertFalse(payload["supervisor_state_present"])
            self.assertEqual(payload["worker_restart_count"], 0)
            self.assertIsNone(payload["last_worker_exit_code"])

    def test_health_json_surfaces_last_successful_restore_time(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            self._write_healthy_status(
                runtime_home,
                last_successful_restore_time="2026-05-18T09:30:15",
            )
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            _, payload, _ = self._run_health(config_path)

            self.assertEqual(
                payload["last_successful_restore_time"], "2026-05-18T09:30:15"
            )

    def test_health_persists_control_health_json(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            self._write_healthy_status(runtime_home)
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            exit_code, payload, _ = self._run_health(config_path)

            persisted_path = runtime_home / "control_health.json"
            self.assertTrue(persisted_path.exists())
            persisted = json.loads(persisted_path.read_text(encoding="utf-8"))
            self.assertEqual(
                persisted["last_health_state"], payload["health_state"]
            )
            self.assertEqual(persisted["last_health_exit_code"], exit_code)
            self.assertTrue(persisted["last_health_time"])

    def test_status_json_uses_health_contract(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            result = _run_control("--status", "--json", "--config", str(config_path))

            self.assertEqual(result.returncode, 2)
            payload = json.loads(result.stdout)
            self.assertEqual(payload["health_state"], "stopped")
