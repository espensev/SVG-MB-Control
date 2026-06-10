from __future__ import annotations

from tests.helpers import *


class EvidenceLogTests(WindowsExeTestCase):
    def test_evidence_log_writes_separate_runtime_artifacts_only(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                poll_ms=100,
                evidence_gpu_sample_mode="full",
            )
            with RuntimeProbe(
                ["--mode", "evidence-log", "--config", str(config_path)],
                env=_sim_direct_env(
                    channel=2,
                    amd_temp_c=76.5,
                    duty_raw=80,
                    mode_raw=5,
                    read_channel=2,
                    read_duty_raw=188,
                    read_mode_raw=7,
                )
                | {
                    "SVG_MB_CONTROL_SIM_READ_FAN_TACH_HI_RAW": "4",
                    "SVG_MB_CONTROL_SIM_READ_FAN_TACH_LO_RAW": "18",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE_COUNT": "2",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE0_LABEL": "sim-vcore",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE0_RAW": "150",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE0_V": "1.2",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE1_LABEL": "sim-3v3",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE1_RAW": "206",
                    "SVG_MB_CONTROL_SIM_SIO_VOLTAGE1_V": "3.296",
                    "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE_COUNT": "1",
                    "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE0_LABEL": "sim-sio-temp0",
                    "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE0_RAW": "61",
                    "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE0_HALF_RAW": "128",
                    "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE0_C": "61.5",
                    "SVG_MB_CONTROL_SIM_GPU_MODE": "enabled",
                },
            ) as probe:
                evidence_csv = runtime_home / "logs" / "svg_mb_control_evidence.csv"
                rows = _wait_for(
                    lambda: _read_runtime_csv_rows(evidence_csv),
                    timeout_s=5.0,
                )
                self.assertTrue(rows, msg="evidence CSV never received rows")
                row = rows[-1]
                self.assertEqual(row["mode"], "evidence-log")
                self.assertEqual(row["amd_sensor_count"], "1")
                self.assertIn("Tctl/Tdie=76.500", row["amd_sensor_summary"])
                self.assertEqual(row["cpu_tctl_c"], "76.500")
                self.assertEqual(row["gpu_available"], "true")
                self.assertEqual(row["gpu_name"], "Simulated NVIDIA GPU")
                self.assertEqual(row["gpu_core_c"], "62.500")
                self.assertEqual(row["gpu_memjn_c"], "72.250")
                self.assertEqual(row["gpu_hotspot_c"], "80.750")
                self.assertEqual(row["fan_count"], "1")
                self.assertEqual(row["fan2_present"], "true")
                self.assertEqual(row["fan2_label"], "sim-channel-2")
                self.assertEqual(row["fan2_tach_raw"], "146")
                self.assertEqual(row["fan2_tach_hi_raw"], "4")
                self.assertEqual(row["fan2_tach_lo_raw"], "18")
                self.assertEqual(row["fan2_duty_raw"], "188")
                self.assertEqual(row["fan2_mode_raw"], "7")
                self.assertEqual(row["telemetry_available"], "true")
                self.assertGreaterEqual(int(row["successful_polls"]), 1)
                self.assertEqual(row["status_detail"], "direct sample captured")
                self.assertIn("evidence_poll_interval_ms", row)
                timing_fields = [
                    "evidence_sample_duration_ms",
                    "runtime_snapshot_read_ms",
                    "amd_read_ms",
                    "gpu_thermal_read_ms",
                    "fan_scan_read_ms",
                    "fan_tach_read_ms",
                    "sio_voltage_read_ms",
                    "sio_temperature_read_ms",
                    "gpu_evidence_read_ms",
                ]
                for field in timing_fields:
                    self.assertNotEqual(row[field], "", msg=field)
                    self.assertGreaterEqual(float(row[field]), 0.0, msg=field)
                change_fields = [
                    "runtime_snapshot_changed",
                    "amd_changed",
                    "gpu_thermal_changed",
                    "fan_state_changed",
                    "fan_tach_changed",
                    "sio_voltage_changed",
                    "sio_temperature_changed",
                    "gpu_evidence_changed",
                ]
                for field in change_fields:
                    self.assertIn(row[field], ("true", "false"), msg=field)
                self.assertEqual(row["sio_evidence_available"], "true")
                self.assertEqual(
                    row["sio_evidence_detail"],
                    "SIO fan tach, voltage, and temperature evidence captured",
                )
                self.assertEqual(row["sio_voltage_count"], "2")
                self.assertEqual(row["sio_voltage0_present"], "true")
                self.assertEqual(row["sio_voltage0_label"], "sim-vcore")
                self.assertEqual(row["sio_voltage0_raw"], "150")
                self.assertEqual(row["sio_voltage0_v"], "1.2000")
                self.assertEqual(row["sio_voltage1_present"], "true")
                self.assertEqual(row["sio_voltage1_label"], "sim-3v3")
                self.assertEqual(row["sio_voltage1_raw"], "206")
                self.assertEqual(row["sio_voltage1_v"], "3.2960")
                self.assertEqual(row["sio_temperature_count"], "1")
                self.assertEqual(row["sio_temp0_present"], "true")
                self.assertEqual(row["sio_temp0_label"], "sim-sio-temp0")
                self.assertEqual(row["sio_temp0_raw"], "61")
                self.assertEqual(row["sio_temp0_half_raw"], "128")
                self.assertEqual(row["sio_temp0_valid"], "true")
                self.assertEqual(row["sio_temp0_c"], "61.500")
                self.assertEqual(row["gpu_evidence_available"], "true")
                self.assertEqual(row["gpu_evidence_sample_mode"], "full")
                self.assertEqual(row["gpu_evidence_requested_sample_mode"], "full")
                self.assertEqual(
                    row["gpu_evidence_detail"],
                    "simulated GPU evidence captured",
                )
                self.assertEqual(row["gpu_evidence_gpu_name"], "Simulated NVIDIA GPU")
                self.assertEqual(row["gpu_evidence_index"], "0")
                self.assertEqual(row["gpu_evidence_time_ms"], "1234")
                self.assertEqual(row["gpu_evidence_dt_ms"], "16")
                self.assertEqual(row["gpu_evidence_core_c"], "62.500")
                self.assertEqual(row["gpu_evidence_memjn_c"], "72.250")
                self.assertEqual(row["gpu_evidence_hotspot_c"], "80.750")
                self.assertEqual(row["gpu_evidence_nvml_temp_c"], "63")
                self.assertEqual(row["gpu_evidence_clock_graphics_mhz"], "2500")
                self.assertEqual(row["gpu_evidence_clock_memory_mhz"], "10500")
                self.assertEqual(row["gpu_evidence_clock_video_mhz"], "1800")
                self.assertEqual(row["gpu_evidence_clock_boost_mhz"], "2700")
                self.assertEqual(row["gpu_evidence_nvml_clock_graphics_mhz"], "2490")
                self.assertEqual(row["gpu_evidence_nvml_clock_memory_mhz"], "10490")
                self.assertEqual(row["gpu_evidence_nvml_clock_video_mhz"], "1790")
                self.assertEqual(row["gpu_evidence_pstate"], "0")
                self.assertEqual(row["gpu_evidence_util_gpu_pct"], "67")
                self.assertEqual(row["gpu_evidence_util_fb_pct"], "21")
                self.assertEqual(row["gpu_evidence_util_vid_pct"], "4")
                self.assertEqual(row["gpu_evidence_nvml_util_gpu_pct"], "68")
                self.assertEqual(row["gpu_evidence_nvml_util_mem_pct"], "22")
                self.assertEqual(row["gpu_evidence_nvml_encoder_pct"], "3")
                self.assertEqual(row["gpu_evidence_nvml_decoder_pct"], "2")
                self.assertEqual(row["gpu_evidence_vram_used_mb"], "8192")
                self.assertEqual(row["gpu_evidence_vram_free_mb"], "8192")
                self.assertEqual(row["gpu_evidence_vram_total_mb"], "16384")
                self.assertEqual(row["gpu_evidence_nvml_power_mw"], "275000")
                self.assertEqual(row["gpu_evidence_power_source"], "nvml")
                self.assertEqual(row["gpu_evidence_voltage_core_mv"], "975")
                self.assertEqual(row["gpu_evidence_pcie_tx_kb_s"], "1024")
                self.assertEqual(row["gpu_evidence_pcie_rx_kb_s"], "2048")
                self.assertEqual(row["gpu_evidence_pcie_link_gen"], "5")
                self.assertEqual(row["gpu_evidence_pcie_link_width"], "16")
                self.assertEqual(row["gpu_evidence_throttle_reasons"], "8")
                self.assertEqual(row["gpu_evidence_fan_count"], "1")
                self.assertEqual(row["gpu_evidence_fan0_level_pct"], "44")
                self.assertEqual(row["gpu_evidence_fan0_rpm"], "1550")
                self.assertEqual(row["gpu_evidence_power_rail_count"], "1")
                self.assertEqual(row["gpu_evidence_power_rail0_domain"], "0")
                self.assertEqual(row["gpu_evidence_power_rail0_power_mw"], "123000")
                self.assertEqual(row["gpu_evidence_thermal_slot_count"], "2")
                self.assertEqual(row["gpu_evidence_thermal_slot0_raw"], "16000")
                self.assertEqual(row["gpu_evidence_thermal_slot1_raw"], "18496")

                evidence_events = runtime_home / "logs" / "svg_mb_control_evidence_events.jsonl"
                events = _wait_for(
                    lambda: _read_jsonl(evidence_events),
                    timeout_s=5.0,
                )
                self.assertTrue(
                    any(
                        event.get("event_type") == "evidence_log.start"
                        for event in events
                    ),
                    msg=events,
                )
                self.assertFalse(
                    (runtime_home / "current_state.json").exists(),
                    msg="evidence-log should not publish controller current_state.json",
                )
                self.assertFalse(
                    (runtime_home / "control_runtime.json").exists(),
                    msg="evidence-log should not publish controller runtime status",
                )
                self.assertFalse(
                    (runtime_home / "logs" / "svg_mb_control_output.csv").exists(),
                    msg="evidence-log should not overwrite default controller CSV",
                )
                self.assertFalse(
                    (runtime_home / "logs" / "svg_mb_control_events.jsonl").exists(),
                    msg="evidence-log should not overwrite default controller events",
                )
                code, _, stderr = probe.stop()

            self.assertEqual(code, 0, msg=stderr)
            manifest = _read_json(
                runtime_home / "logs" / "svg_mb_control_evidence_manifest.json"
            )
            self.assertIsNotNone(manifest)
            self.assertEqual(manifest["status"], "completed")
            self.assertEqual(manifest["mode"], "evidence-log")
            self.assertGreaterEqual(manifest["row_count"], 1)
            self.assertEqual(manifest["rows_written"], manifest["row_count"])
            self.assertIsNotNone(manifest["session_stop"])
            self.assertEqual(
                manifest["artifacts"]["csv_latest"]["path"],
                str(runtime_home / "logs" / "svg_mb_control_evidence.csv"),
            )
            self.assertEqual(
                manifest["artifacts"]["events"]["path"],
                str(runtime_home / "logs" / "svg_mb_control_evidence_events.jsonl"),
            )
            self.assertEqual(
                manifest["artifacts"]["manifest_latest"]["path"],
                str(runtime_home / "logs" / "svg_mb_control_evidence_manifest.json"),
            )
            events = _read_jsonl(
                runtime_home / "logs" / "svg_mb_control_evidence_events.jsonl"
            )
            self.assertEqual(manifest["event_count"], len(events))
            self.assertEqual(manifest["events_written"], len(events))
            self.assertTrue(
                any(
                    event.get("event_type") == "evidence_log.shutdown"
                    for event in events
                ),
                msg=events,
            )
