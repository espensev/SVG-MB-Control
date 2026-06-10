from __future__ import annotations

from tests.helpers import *


class CalibrationTests(WindowsExeTestCase):
    def test_calibrate_writes_plant_model_capture(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                baseline_freshness_ceiling_ms=10000,
                restore_timeout_ms=5000,
            )
            output_path = runtime_home / "plant_model.json"
            result = _run_control(
                "--mode",
                "calibrate",
                "--config",
                str(config_path),
                "--calibrate-channel",
                "0",
                "--calibrate-step-ms",
                "200",
                "--calibrate-cooldown-ms",
                "200",
                "--calibrate-settle-window-ms",
                "100",
                "--calibrate-output",
                str(output_path),
                env=_sim_direct_env(channel=0, amd_temp_c=60.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            self.assertTrue(output_path.is_file(), msg="plant_model.json missing")
            payload = _read_json(output_path)
            self.assertIsNotNone(payload)
            self.assertEqual(payload["schema"], "svg_mb_control.plant_model_capture.v1")
            self.assertEqual(payload["schema_version"], 1)
            self.assertIsNone(payload["abort_reason"])
            self.assertIn("producer", payload)
            self.assertEqual(payload["producer"]["tool"], "svg-mb-control")
            self.assertIn("options", payload)
            self.assertIn("sequence", payload["options"])
            self.assertEqual(payload["options"]["settle_window_ms"], 100)
            self.assertEqual(len(payload["channels"]), 1)
            channel = payload["channels"][0]
            self.assertEqual(channel["channel"], 0)
            self.assertTrue(channel["baseline_captured"])
            self.assertTrue(channel["restored"])
            self.assertIn("baseline", channel)
            self.assertIn("duty_raw", channel["baseline"])
            self.assertIn("mode_raw", channel["baseline"])
            self.assertEqual(len(channel["steps"]), 6)
            first_step = channel["steps"][0]
            self.assertEqual(first_step["hold_ms"], 200)
            self.assertEqual(first_step["settle_window_ms"], 100)
            self.assertEqual(first_step["duty_pct_target"], 20.0)
            self.assertIn("rpm_mean", first_step)
            self.assertIn("tctl_c_mean", first_step)
            self.assertIn("settle_sample_count", first_step)

    def test_calibrate_refuses_when_no_writable_channels(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
            )
            output_path = runtime_home / "plant_model.json"
            result = _run_control(
                "--mode",
                "calibrate",
                "--config",
                str(config_path),
                "--calibrate-channel",
                "99",
                "--calibrate-step-ms",
                "200",
                "--calibrate-cooldown-ms",
                "200",
                "--calibrate-output",
                str(output_path),
                env=_sim_direct_env(channel=0, amd_temp_c=60.0),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no writable channels", result.stderr)

    def test_calibrate_accepts_custom_duty_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
                baseline_freshness_ceiling_ms=10000,
                restore_timeout_ms=5000,
            )
            output_path = runtime_home / "plant_model.json"
            result = _run_control(
                "--mode",
                "calibrate",
                "--config",
                str(config_path),
                "--calibrate-channel",
                "0",
                "--calibrate-sequence",
                "22:200,24:300,26:200",
                "--calibrate-settle-window-ms",
                "100",
                "--calibrate-output",
                str(output_path),
                env=_sim_direct_env(channel=0, amd_temp_c=60.0),
            )
            self.assertEqual(result.returncode, 0, msg=f"{result.stdout}\n{result.stderr}")
            payload = _read_json(output_path)
            sequence = payload["options"]["sequence"]
            self.assertEqual(
                sequence,
                [
                    {"duty_pct": 22.0, "hold_ms": 200},
                    {"duty_pct": 24.0, "hold_ms": 300},
                    {"duty_pct": 26.0, "hold_ms": 200},
                ],
            )
            steps = payload["channels"][0]["steps"]
            self.assertEqual([step["duty_pct_target"] for step in steps], [22.0, 24.0, 26.0])

    def test_calibrate_rejects_bad_custom_duty_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            runtime_home.mkdir(parents=True, exist_ok=True)
            config_path = _write_write_once_config(
                td,
                runtime_home=runtime_home,
            )
            result = _run_control(
                "--mode",
                "calibrate",
                "--config",
                str(config_path),
                "--calibrate-channel",
                "0",
                "--calibrate-sequence",
                "52,54",
                env=_sim_direct_env(channel=0, amd_temp_c=60.0),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Invalid --calibrate-sequence", result.stderr)
