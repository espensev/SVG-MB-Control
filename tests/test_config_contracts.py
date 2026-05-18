from __future__ import annotations

from tests.helpers import *


class ConfigContractTests(unittest.TestCase):
    def test_shipped_configs_default_to_control_loop(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            self.assertEqual(
                payload["default_mode"],
                "control-loop",
                msg=f"{rel_path} should start normal control on plain launch",
            )

    def test_shipped_control_loop_configs_use_smooth_step_cadence(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            self.assertEqual(
                payload["control_loop"]["poll_tick_ms"],
                250,
                msg=f"{rel_path} control-loop tick drifted",
            )
            self.assertEqual(
                payload["control_loop"]["write_cooldown_ms"],
                250,
                msg=f"{rel_path} write cooldown drifted",
            )
            self.assertLessEqual(
                payload["control_loop"]["deadband_pct"],
                0.25,
                msg=f"{rel_path} deadband should allow sub-1% step writes",
            )
            by_channel = {
                item["channel"]: item
                for item in payload["control_loop"]["channels"]
            }
            self.assertLessEqual(by_channel[0]["min_duty_pct"], 16.0)
            self.assertLessEqual(by_channel[1]["min_duty_pct"], 20.0)
            self.assertGreaterEqual(by_channel[2]["min_duty_pct"], 54.0)
            self.assertGreaterEqual(by_channel[3]["min_duty_pct"], 50.0)
            self.assertGreaterEqual(
                by_channel[2]["min_duty_pct"] - by_channel[3]["min_duty_pct"],
                4.0,
                msg=f"{rel_path} front 200mm channels should avoid same-rpm resonance",
            )
            self.assertLessEqual(by_channel[4]["min_duty_pct"], 28.0)
            self.assertLessEqual(by_channel[5]["min_duty_pct"], 18.0)
            for channel in by_channel.values():
                self.assertIn("max_setpoint_step_pct", channel)
                self.assertGreater(channel["max_setpoint_step_pct"], 0.0)
                self.assertLess(
                    channel["max_setpoint_step_pct"],
                    1.0,
                    msg=(
                        f"{rel_path} channel {channel['channel']} "
                        "must emit sub-1% setpoint steps"
                    ),
                )

    def test_shipped_control_loop_configs_scope_to_live_airflow_lanes(self) -> None:
        expected_channels = [0, 1, 2, 3, 4, 5]
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            channels = sorted(
                item["channel"]
                for item in payload["control_loop"]["channels"]
            )
            self.assertEqual(
                channels,
                expected_channels,
                msg=f"{rel_path} channel rollout drifted",
            )
            self.assertEqual(
                payload["control_loop"]["control_hold_ms"],
                0,
                msg=f"{rel_path} should hold control writes until shutdown",
            )

    def test_shipped_control_loop_configs_include_cpu_response_overlay(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            for channel in payload["control_loop"]["channels"]:
                overlay = channel.get("cpu_override_curve")
                self.assertIsInstance(
                    overlay,
                    list,
                    msg=f"{rel_path} channel {channel['channel']} missing CPU overlay",
                )
                self.assertGreaterEqual(
                    len(overlay),
                    2,
                    msg=f"{rel_path} channel {channel['channel']} CPU overlay too small",
                )

    def test_shipped_control_loop_configs_include_smooth_decay_controls(self) -> None:
        required = {
            "demand_smoothing_rise_alpha",
            "demand_smoothing_fall_alpha",
            "decay_latch_above_pct",
            "decay_latch_pct_per_min",
        }
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            for channel in payload["control_loop"]["channels"]:
                missing = sorted(required.difference(channel))
                self.assertEqual(
                    missing,
                    [],
                    msg=f"{rel_path} channel {channel['channel']} missing smoothing keys",
                )

    def test_shipped_radiator_lanes_include_thermal_pressure_boost(self) -> None:
        required = {
            "thermal_pressure_start_c",
            "thermal_pressure_full_c",
            "thermal_pressure_rise_pct_per_sec",
            "thermal_pressure_fall_pct_per_sec",
            "thermal_pressure_max_boost_pct",
        }
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            by_channel = {
                item["channel"]: item
                for item in payload["control_loop"]["channels"]
            }
            for channel_id in (1, 4, 5):
                channel = by_channel[channel_id]
                missing = sorted(required.difference(channel))
                self.assertEqual(
                    missing,
                    [],
                    msg=f"{rel_path} channel {channel_id} missing pressure keys",
                )
                self.assertLessEqual(channel["thermal_pressure_start_c"], 86.5)
                self.assertGreaterEqual(
                    channel["thermal_pressure_max_boost_pct"], 14.0
                )
                self.assertLessEqual(channel["decay_latch_pct_per_min"], 90.0)
                self.assertGreater(
                    channel["thermal_pressure_rise_pct_per_sec"],
                    channel["thermal_pressure_fall_pct_per_sec"],
                )

    def test_shipped_radiator_lanes_include_cpu_low_soak(self) -> None:
        required = {
            "cpu_low_soak_start_c",
            "cpu_low_soak_full_c",
            "cpu_low_soak_release_c",
            "cpu_low_soak_rise_pct_per_min",
            "cpu_low_soak_fall_pct_per_min",
            "cpu_low_soak_max_boost_pct",
        }
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            by_channel = {
                item["channel"]: item
                for item in payload["control_loop"]["channels"]
            }
            self.assertEqual(
                sorted(
                    channel_id
                    for channel_id, channel in by_channel.items()
                    if "cpu_low_soak_start_c" in channel
                ),
                [1, 4, 5],
            )
            for channel_id in (1, 4, 5):
                channel = by_channel[channel_id]
                missing = sorted(required.difference(channel))
                self.assertEqual(
                    missing,
                    [],
                    msg=f"{rel_path} channel {channel_id} missing CPU low soak keys",
                )
                self.assertGreaterEqual(channel["cpu_low_soak_start_c"], 66.0)
                self.assertGreaterEqual(channel["cpu_low_soak_full_c"], 78.0)
                self.assertLessEqual(
                    channel["cpu_low_soak_release_c"],
                    channel["cpu_low_soak_start_c"],
                )
                self.assertLessEqual(channel["cpu_low_soak_max_boost_pct"], 0.8)

    def test_shipped_low_band_stage_policy_is_slow_and_staged(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            low_band = payload["control_loop"]["low_band"]
            self.assertTrue(low_band["enabled"])
            self.assertEqual(low_band["cpu_start_c"], 62.0)
            self.assertEqual(low_band["cpu_full_c"], 71.0)
            self.assertEqual(low_band["gpu_start_c"], 68.0)
            self.assertEqual(low_band["gpu_full_c"], 76.0)
            self.assertLessEqual(low_band["rise_per_min"], 0.18)
            self.assertLessEqual(low_band["stage_rise_pct_per_min"], 0.8)
            self.assertGreaterEqual(low_band["stage_spacing_ms"], 60000)
            self.assertGreaterEqual(
                low_band["evidence_write_interval_ms"], 1000
            )

            by_channel = {
                item["channel"]: item
                for item in payload["control_loop"]["channels"]
            }
            expected = {
                2: (1, 0.25, 120000, 2.0),
                3: (1, 0.25, 120000, 2.0),
                1: (2, 0.45, 180000, 1.8),
                4: (3, 0.65, 240000, 1.2),
                5: (3, 0.65, 240000, 1.2),
            }
            for channel_id, (stage, threshold, hold_ms, cap) in expected.items():
                channel = by_channel[channel_id]
                self.assertEqual(channel["low_band_stage"], stage)
                self.assertEqual(channel["low_band_debt_threshold"], threshold)
                self.assertEqual(channel["low_band_hold_ms"], hold_ms)
                self.assertLessEqual(channel["low_band_max_boost_pct"], cap)
            self.assertNotIn("low_band_stage", by_channel[0])

    def test_shipped_noctua_cpu_overlays_are_staggered(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            by_channel = {
                item["channel"]: item["cpu_override_curve"]
                for item in payload["control_loop"]["channels"]
            }
            self.assertNotEqual(by_channel[1], by_channel[4])
            self.assertNotEqual(by_channel[1], by_channel[5])
            self.assertNotEqual(by_channel[4], by_channel[5])

    def test_shipped_live_runtime_policy_blocks_channel_6(self) -> None:
        payload = _read_json(REPO_ROOT / "config" / "runtime_policy_write_live.json")
        self.assertIsNotNone(payload)
        self.assertEqual(payload["control"]["blocked_channels"], [6])
