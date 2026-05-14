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

    def test_shipped_control_loop_configs_use_characterized_tick(self) -> None:
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            self.assertEqual(
                payload["control_loop"]["poll_tick_ms"],
                50,
                msg=f"{rel_path} control-loop tick drifted",
            )
            self.assertEqual(
                payload["control_loop"]["write_cooldown_ms"],
                50,
                msg=f"{rel_path} write cooldown drifted",
            )
            self.assertLessEqual(
                payload["control_loop"]["deadband_pct"],
                0.4,
                msg=f"{rel_path} deadband should allow one-step writes",
            )

    def test_shipped_control_loop_configs_scope_to_live_airflow_lanes(self) -> None:
        expected_channels = [0, 1, 2, 3, 4, 5]
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            channels = [
                item["channel"]
                for item in payload["control_loop"]["channels"]
            ]
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
                self.assertLessEqual(channel["thermal_pressure_start_c"], 82.5)
                self.assertGreaterEqual(
                    channel["thermal_pressure_max_boost_pct"], 20.0
                )
                self.assertLessEqual(channel["decay_latch_pct_per_min"], 180.0)
                self.assertGreater(
                    channel["thermal_pressure_rise_pct_per_sec"],
                    channel["thermal_pressure_fall_pct_per_sec"],
                )

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
