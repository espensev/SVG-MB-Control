from __future__ import annotations

from tests.helpers import *


class MachineCoolingPolicyTests(unittest.TestCase):
    POLICY_PATH = REPO_ROOT / "config" / "machines" / "snd-desk.cooling.policy.json"

    def _policy(self) -> dict:
        payload = _read_json(self.POLICY_PATH)
        self.assertIsNotNone(payload, msg=f"missing policy: {self.POLICY_PATH}")
        return payload

    def _fans_by_channel(self) -> dict[int, dict]:
        return {fan["channel"]: fan for fan in self._policy()["fans"]}

    def test_policy_records_positive_pressure_strategy(self) -> None:
        policy = self._policy()
        self.assertEqual(policy["schema"], "svg_mb_control.machine_cooling_policy.v1")
        self.assertEqual(policy["schema_version"], 2)
        self.assertEqual(policy["machine"]["source_doc"], "docs/COOLING_STRATEGY.md")
        self.assertEqual(
            policy["machine"]["profile_doc"],
            "docs/NORMAL_RUNTIME_AIRFLOW_PROFILE.md",
        )
        self.assertEqual(policy["release_profile"]["soft_floor_channels"], [2, 3, 4])

        strategy = policy["strategy"]
        idle = strategy["idle_low_load"]
        high = strategy["high_load"]
        control = strategy["control_model"]

        self.assertEqual(idle["priority"], "positive_case_pressure")
        self.assertEqual(idle["intake_channels"], [2, 3, 4])
        self.assertEqual(idle["exhaust_channels"], [0, 1, 5])
        self.assertEqual(idle["excluded_channels"], [6])
        self.assertGreaterEqual(idle["target_intake_to_exhaust_flow_index_ratio_min"], 1.1)

        self.assertEqual(high["priority"], "thermal_performance")
        self.assertTrue(high["pressure_balance_may_relax"])

        self.assertTrue(control["avoid_static_high_hard_floors"])
        self.assertTrue(control["use_dynamic_soft_floor_for_low_medium_load"])
        self.assertTrue(control["require_independent_fan_responses"])

    def test_policy_topology_matches_controlled_channels(self) -> None:
        fans = self._fans_by_channel()
        self.assertEqual(sorted(fans), [0, 1, 2, 3, 4, 5, 6])

        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            config = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(config, msg=f"missing config: {rel_path}")
            config_channels = sorted(
                channel["channel"]
                for channel in config["control_loop"]["channels"]
            )
            self.assertEqual(config_channels, [0, 1, 2, 3, 4, 5])
            self.assertEqual(
                config_channels,
                sorted(policy_channel for policy_channel in fans if policy_channel != 6),
                msg=f"{rel_path} must stay covered by the machine cooling policy",
            )

    def test_intake_and_exhaust_roles_are_explicit(self) -> None:
        fans = self._fans_by_channel()
        self.assertEqual(
            [fans[channel]["direction"] for channel in (2, 3, 4)],
            ["intake", "intake", "intake"],
        )
        self.assertEqual(
            [fans[channel]["role"] for channel in (2, 3, 4)],
            ["front_case_intake", "front_case_intake", "front_radiator_intake"],
        )
        self.assertEqual(
            [fans[channel]["fan_model"] for channel in (2, 3)],
            [
                "ASUS ProArt PA602 stock 200 x 38 mm PWM",
                "ASUS ProArt PA602 stock 200 x 38 mm PWM",
            ],
        )
        self.assertEqual(
            [fans[channel]["direction"] for channel in (0, 1, 5)],
            ["exhaust", "exhaust", "exhaust"],
        )
        self.assertEqual(fans[6]["direction"], "excluded")

    def test_reference_static_low_load_profile_is_intake_biased(self) -> None:
        policy = self._policy()
        fans = self._fans_by_channel()
        target_ratio = policy["strategy"]["idle_low_load"][
            "target_intake_to_exhaust_flow_index_ratio_min"
        ]

        def flow_index(channel: int) -> float:
            fan = fans[channel]
            rpm = fan["reference_static_low_load_rpm"]
            diameter = fan["diameter_mm"]
            self.assertIsNotNone(rpm, msg=f"channel {channel} missing rpm")
            self.assertIsNotNone(diameter, msg=f"channel {channel} missing diameter")
            return float(rpm) / 1000.0 * (float(diameter) / 120.0) ** 2

        intake = sum(flow_index(channel) for channel in (2, 3, 4))
        exhaust = sum(flow_index(channel) for channel in (0, 1, 5))
        self.assertGreater(
            intake,
            exhaust * target_ratio,
            msg="reference idle/low-load profile should retain positive pressure bias",
        )

    def test_soft_floor_curves_are_not_high_static_floors(self) -> None:
        fans = self._fans_by_channel()
        for channel in (2, 3, 4):
            fan = fans[channel]
            curve = fan["response_intent"]["soft_floor_curve"]
            self.assertGreaterEqual(len(curve), 3)
            self.assertLess(
                curve[0]["duty_pct"],
                fan["reference_static_low_load_duty_pct"],
                msg=f"channel {channel} candidate should reduce static low-load floor",
            )
            self.assertEqual(curve[0]["duty_pct"], fan["release_min_duty_pct"])
            for prev, cur in zip(curve, curve[1:]):
                self.assertGreater(cur["temp_c"], prev["temp_c"])
                self.assertGreaterEqual(cur["duty_pct"], prev["duty_pct"])

        ch2_curve = fans[2]["response_intent"]["soft_floor_curve"]
        ch3_curve = fans[3]["response_intent"]["soft_floor_curve"]
        self.assertEqual(
            [point["temp_c"] for point in ch2_curve],
            [point["temp_c"] for point in ch3_curve],
        )
        for ch2, ch3 in zip(ch2_curve, ch3_curve):
            self.assertGreaterEqual(
                ch2["duty_pct"] - ch3["duty_pct"],
                4.0,
                msg="front 200 mm intake candidates should keep resonance spacing",
            )

    def test_radiator_exhaust_pair_is_not_mirrored_and_stays_above_rear_exhaust(self) -> None:
        policy = self._policy()
        fans = self._fans_by_channel()
        group = policy["groups"]["radiator_exhaust_pair"]

        self.assertEqual(group["channels"], [1, 5])
        self.assertFalse(group["allow_mirrored_response"])
        self.assertGreater(
            fans[1]["release_min_duty_pct"],
            fans[0]["release_min_duty_pct"],
        )
        self.assertGreater(
            fans[5]["release_min_duty_pct"],
            fans[0]["release_min_duty_pct"],
        )
        self.assertIsNone(fans[1]["response_intent"]["mirror_group"])
        self.assertIsNone(fans[5]["response_intent"]["mirror_group"])


if __name__ == "__main__":
    unittest.main()
