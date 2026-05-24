from __future__ import annotations

from tests.helpers import *


def _write_stop_request(runtime_home: Path) -> None:
    runtime_home.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "requested_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "reason": "test-cleanup",
    }
    (runtime_home / "stop.request.json").write_text(
        json.dumps(payload), encoding="utf-8"
    )


class RuntimeSingletonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_second_worker_against_same_runtime_home_refuses_to_start(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                poll_ms=100,
            )

            first = _spawn_control(
                ["--mode", "read-loop", "--config", str(config_path)],
                env=_sim_direct_env(),
            )
            try:
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(
                    status, msg="first worker never wrote control_runtime.json"
                )
                first_pid = status["process_id"]
                self.assertNotEqual(first_pid, 0)

                second = subprocess.run(
                    [
                        str(CONTROL_EXE),
                        "--mode",
                        "read-loop",
                        "--config",
                        str(config_path),
                    ],
                    capture_output=True,
                    text=True,
                    env=_merged_env(_sim_direct_env()),
                    timeout=10.0,
                )

                self.assertNotEqual(
                    second.returncode,
                    0,
                    msg=(
                        "second worker did not refuse start: "
                        f"stdout={second.stdout!r} stderr={second.stderr!r}"
                    ),
                )
                self.assertIn(
                    "already running",
                    (second.stdout + second.stderr).lower(),
                )

                status_after = _read_runtime_status(runtime_home)
                self.assertIsNotNone(status_after)
                self.assertEqual(
                    status_after["process_id"],
                    first_pid,
                    msg="second worker overwrote control_runtime.json",
                )
            finally:
                _stop_and_wait(first)

    def test_second_supervisor_against_same_runtime_home_refuses_to_start(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                poll_ms=100,
            )

            first = _spawn_control(
                [
                    "--run-supervisor",
                    "--mode",
                    "read-loop",
                    "--config",
                    str(config_path),
                ],
                env=_sim_direct_env(),
            )
            try:
                sup_state = _wait_for(
                    lambda: _read_json(runtime_home / "control_supervisor.json"),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(
                    sup_state,
                    msg="first supervisor never wrote control_supervisor.json",
                )
                first_sup_pid = sup_state["supervisor_pid"]
                self.assertNotEqual(first_sup_pid, 0)

                second = subprocess.run(
                    [
                        str(CONTROL_EXE),
                        "--run-supervisor",
                        "--mode",
                        "read-loop",
                        "--config",
                        str(config_path),
                    ],
                    capture_output=True,
                    text=True,
                    env=_merged_env(_sim_direct_env()),
                    timeout=10.0,
                )

                self.assertNotEqual(
                    second.returncode,
                    0,
                    msg=(
                        "second supervisor did not refuse start: "
                        f"stdout={second.stdout!r} stderr={second.stderr!r}"
                    ),
                )
                self.assertIn(
                    "already running",
                    (second.stdout + second.stderr).lower(),
                )

                sup_after = _read_json(runtime_home / "control_supervisor.json")
                self.assertIsNotNone(sup_after)
                self.assertEqual(
                    sup_after["supervisor_pid"],
                    first_sup_pid,
                    msg="second supervisor overwrote control_supervisor.json",
                )
            finally:
                _write_stop_request(runtime_home)
                try:
                    first.communicate(timeout=15.0)
                except subprocess.TimeoutExpired:
                    first.kill()
                    first.communicate()
