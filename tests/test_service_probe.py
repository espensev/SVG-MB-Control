from __future__ import annotations

from tests.helpers import *


class ServiceProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_service_probe_json_passes_under_simulation(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            result = _run_control(
                "--service-probe",
                "--json",
                "--config",
                str(config_path),
                env=_sim_direct_env(),
            )

            self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
            payload = json.loads(result.stdout)
            self.assertEqual(payload["schema_version"], 1)
            self.assertEqual(payload["overall"], "pass")

            checks = {c["name"]: c for c in payload["checks"]}
            for required_name in (
                "amd_telemetry",
                "sio_fan_state",
                "runtime_home_writable",
                "no_write_lifecycle",
            ):
                self.assertIn(required_name, checks)
                self.assertTrue(checks[required_name]["required"])
                self.assertTrue(
                    checks[required_name]["ok"],
                    msg=f"{required_name}: {checks[required_name]['detail']}",
                )

            # GPU is advisory: present, not required, and never fails overall.
            self.assertIn("gpu_telemetry", checks)
            self.assertFalse(checks["gpu_telemetry"]["required"])

    def test_service_probe_makes_no_fan_writes(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(td, runtime_home=runtime_home)

            result = _run_control(
                "--service-probe",
                "--config",
                str(config_path),
                env=_sim_direct_env(),
            )

            self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
            self.assertIn("service-probe pass", result.stdout)
            self.assertFalse(
                (runtime_home / "pending_writes.json").exists(),
                msg="probe must not create a pending-write sidecar",
            )

    def test_service_probe_rejects_bare_json(self) -> None:
        result = _run_control("--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--service-probe", result.stderr + result.stdout)
