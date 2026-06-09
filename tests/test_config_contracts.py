from __future__ import annotations

from tests.helpers import *


class ConfigContractTests(unittest.TestCase):
    def test_native_watchdog_recovery_uses_restart(self) -> None:
        source = (REPO_ROOT / "src" / "platform" / "task_runner.cpp").read_text(
            encoding="utf-8"
        )
        watchdog_block = source.split(
            'if (HasFlag(argc, argv, L"--watchdog-run")) {', 1
        )[1].split('if (HasFlag(argc, argv, L"--start")) {', 1)[0]
        self.assertIn(
            'RunHiddenAndWait(control_exe, {L"--restart"}, config_path)',
            watchdog_block,
        )
        self.assertNotIn(
            'RunHiddenAndWait(control_exe, {L"--start"}, config_path)',
            watchdog_block,
        )

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
            self.assertLessEqual(by_channel[1]["min_duty_pct"], 22.0)
            if rel_path.name == "control.release.json":
                self.assertEqual(by_channel[2]["min_duty_pct"], 42.0)
                self.assertEqual(by_channel[3]["min_duty_pct"], 38.0)
                self.assertEqual(by_channel[4]["min_duty_pct"], 24.0)
            else:
                self.assertGreaterEqual(by_channel[2]["min_duty_pct"], 54.0)
                self.assertGreaterEqual(by_channel[3]["min_duty_pct"], 50.0)
                self.assertLessEqual(by_channel[4]["min_duty_pct"], 31.0)
            self.assertGreaterEqual(
                by_channel[2]["min_duty_pct"] - by_channel[3]["min_duty_pct"],
                4.0,
                msg=f"{rel_path} front 200mm channels should avoid same-rpm resonance",
            )
            self.assertLessEqual(by_channel[5]["min_duty_pct"], 20.0)
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

    def test_release_intake_low_end_curves_follow_machine_policy(self) -> None:
        policy = _read_json(
            REPO_ROOT / "config" / "machines" / "snd-desk.cooling.policy.json"
        )
        self.assertIsNotNone(policy)
        config = _read_json(REPO_ROOT / "config" / "control.release.json")
        self.assertIsNotNone(config)

        release_channels = {
            item["channel"]: item
            for item in config["control_loop"]["channels"]
        }
        fans = {fan["channel"]: fan for fan in policy["fans"]}

        for channel_id in policy["release_profile"]["soft_floor_channels"]:
            fan = fans[channel_id]
            expected_curve = fan["response_intent"]["soft_floor_curve"]
            channel = release_channels[channel_id]
            self.assertEqual(channel["min_duty_pct"], fan["release_min_duty_pct"])
            self.assertEqual(channel["min_duty_pct"], expected_curve[0]["duty_pct"])
            self.assertEqual(
                channel["curve"][: len(expected_curve)],
                expected_curve,
                msg=f"channel {channel_id} primary low-end curve drifted",
            )
            self.assertEqual(
                channel["cpu_override_curve"][: len(expected_curve)],
                expected_curve,
                msg=f"channel {channel_id} CPU low-end curve drifted",
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

    def test_shipped_max_blend_channels_are_source_aware_with_cpu_guard(self) -> None:
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
            # All controlled lanes are source-aware: intakes (0,2,3,4) and the
            # radiator exhausts (1,5), which gained a GPU-airflow assist curve
            # on 2026-06-09 so their setpoint ramps with GPU temperature alongside
            # the GPU intakes. The CPU exhaust response is carried through each
            # lane's cpu_override_curve (see the decision record).
            for channel_id in (0, 1, 2, 3, 4, 5):
                channel = by_channel[channel_id]
                self.assertEqual(
                    channel["temp_blend"],
                    "max_cpu_gpu_source_aware",
                    msg=f"{rel_path} channel {channel_id} blend drifted",
                )
                self.assertEqual(
                    channel["source_aware_cpu_hot_guard_c"],
                    75.0,
                    msg=f"{rel_path} channel {channel_id} guard drifted",
                )

    def test_radiator_exhaust_gpu_curves_are_staggered_not_mirrored(self) -> None:
        # CHA1/CHA5 are same-model NF-A14 radiator exhausts on one radiator;
        # their GPU-airflow assist curves must not arrive at the same duty at
        # the same temperature (same-RPM resonance). They share temp knots and
        # channel 1 stays strictly above channel 5 by >= 2 pts at every knot.
        # The intake pair has an analogous >= 4 pt guard in
        # test_machine_cooling_policy; allow_mirrored_response=false has no
        # teeth on the GPU curves without this comparison.
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
            c1 = by_channel[1]["curve"]
            c5 = by_channel[5]["curve"]
            self.assertEqual(
                [point["temp_c"] for point in c1],
                [point["temp_c"] for point in c5],
                msg=f"{rel_path} radiator-exhaust GPU curves must share temp knots",
            )
            for p1, p5 in zip(c1, c5):
                self.assertGreaterEqual(
                    p1["duty_pct"] - p5["duty_pct"],
                    2.0,
                    msg=(
                        f"{rel_path} CHA1/CHA5 GPU curves mirror at "
                        f"{p1['temp_c']} C ({p1['duty_pct']} vs "
                        f"{p5['duty_pct']}); stagger to avoid same-RPM resonance"
                    ),
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

    def test_shipped_lanes_include_midband_and_gpu_airflow(self) -> None:
        midband_keys = {
            "midband_pressure_start_c",
            "midband_pressure_full_c",
            "midband_pressure_rise_pct_per_sec",
            "midband_pressure_fall_pct_per_sec",
            "midband_pressure_max_boost_pct",
        }
        gpu_keys = {
            "gpu_airflow_start_c",
            "gpu_airflow_full_c",
            "gpu_airflow_rise_pct_per_sec",
            "gpu_airflow_fall_pct_per_sec",
            "gpu_airflow_max_boost_pct",
        }
        for rel_path in (
            Path("config") / "control.example.json",
            Path("config") / "control.release.json",
        ):
            payload = _read_json(REPO_ROOT / rel_path)
            self.assertIsNotNone(payload, msg=f"missing config: {rel_path}")
            loop = payload["control_loop"]
            cap = loop.get("low_band_residual_cap_pct")
            self.assertIsNotNone(
                cap,
                msg=f"{rel_path} missing low_band_residual_cap_pct",
            )
            self.assertGreater(cap, 0.0)
            self.assertLessEqual(
                cap,
                2.0,
                msg=f"{rel_path} low-band must stay a small residual",
            )
            for channel in loop["channels"]:
                cid = channel["channel"]
                self.assertEqual(
                    sorted(midband_keys.difference(channel)),
                    [],
                    msg=f"{rel_path} channel {cid} missing midband keys",
                )
                self.assertEqual(
                    sorted(gpu_keys.difference(channel)),
                    [],
                    msg=f"{rel_path} channel {cid} missing gpu_airflow keys",
                )
                # Mid-band must engage below the high-temperature
                # thermal-pressure stage (<= 86.5 C elsewhere) and hand off
                # to it.
                self.assertLessEqual(
                    channel["midband_pressure_start_c"], 70.0
                )
                self.assertGreater(
                    channel["midband_pressure_full_c"],
                    channel["midband_pressure_start_c"],
                )
                self.assertLessEqual(
                    channel["midband_pressure_max_boost_pct"], 20.0
                )
                # GPU airflow must start early (tight-band intent) but above
                # true idle.
                self.assertLessEqual(channel["gpu_airflow_start_c"], 64.0)
                self.assertGreaterEqual(channel["gpu_airflow_start_c"], 55.0)
                self.assertGreater(
                    channel["gpu_airflow_full_c"],
                    channel["gpu_airflow_start_c"],
                )
                self.assertLessEqual(
                    channel["gpu_airflow_max_boost_pct"], 20.0
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
