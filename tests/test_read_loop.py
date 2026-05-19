from __future__ import annotations

from tests.helpers import *


class ReadLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform != "win32":
            raise unittest.SkipTest("Windows-only repo")
        _ensure_release_build()

    def test_read_loop_publishes_control_owned_current_state_and_mirror(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            snapshot_path = td / "mirror" / "current_state.json"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                snapshot_path=snapshot_path,
                poll_ms=100,
            )
            proc = _spawn_control(
                ["--mode", "read-loop", "--config", str(config_path)],
                env=_sim_direct_env(
                    channel=0,
                    amd_temp_c=83.0,
                    duty_raw=90,
                    mode_raw=7,
                    read_channel=0,
                    read_duty_raw=222,
                    read_mode_raw=7,
                ),
            )
            try:
                state = _wait_for(
                    lambda: _read_runtime_current_state(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(state, msg="current_state.json never appeared")
                status = _wait_for(
                    lambda: _read_runtime_status(runtime_home),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(status, msg="control_runtime.json never appeared")
                mirror = _wait_for(
                    lambda: _read_json(snapshot_path),
                    timeout_s=5.0,
                )
                self.assertIsNotNone(mirror, msg="snapshot mirror never appeared")

                fan0 = next(fan for fan in state["fans"] if fan["channel"] == 0)
                self.assertEqual(fan0["duty_raw"], 222)
                self.assertEqual(fan0["mode_raw"], 7)
                self.assertEqual(state["amd_sensors"][0]["temperature_c"], 83.0)
                self.assertEqual(mirror["fans"][0]["duty_raw"], 222)
                self.assertGreaterEqual(status["successful_polls"], 1)
                self.assertEqual(status["restart_count"], 0)
                self.assertEqual(status["child_pid"], 0)
                self.assertTrue(status["log_csv_path"])
                self.assertTrue(status["log_manifest_path"])
                self.assertTrue(status["event_log_path"])
                self.assertTrue(Path(status["log_csv_path"]).is_file())
                self.assertTrue(Path(status["log_manifest_path"]).is_file())
                self.assertTrue(Path(status["event_log_path"]).is_file())
                self.assertTrue(_runtime_archive_files(runtime_home))
                self.assertTrue(
                    (runtime_home / "logs" / "svg_mb_control_output.csv").is_file()
                )
                archive_rows = _wait_for(
                    lambda: _read_runtime_csv_rows(Path(status["log_csv_path"])),
                    timeout_s=5.0,
                )
                self.assertTrue(archive_rows, msg="archive csv never received rows")
                mirror_rows = _read_runtime_csv_rows(
                    runtime_home / "logs" / "svg_mb_control_output.csv"
                )
                self.assertTrue(mirror_rows, msg="live mirror csv never received rows")
                archive_row = archive_rows[-1]
                mirror_row = mirror_rows[-1]
                self.assertEqual(archive_row["mode"], "read-loop")
                self.assertEqual(archive_row["amd_sensor_count"], "1")
                self.assertIn("Tctl/Tdie=83.000", archive_row["amd_sensor_summary"])
                self.assertEqual(archive_row["cpu_tctl_c"], "83.000")
                self.assertEqual(archive_row["fan_count"], "1")
                self.assertEqual(archive_row["fan0_present"], "true")
                self.assertEqual(archive_row["fan0_label"], "sim-channel-0")
                self.assertEqual(archive_row["fan0_duty_raw"], "222")
                self.assertEqual(archive_row["fan0_mode_raw"], "7")
                self.assertEqual(archive_row["fan0_write_allowed"], "true")
                self.assertEqual(archive_row["fan0_policy_blocked"], "false")
                self.assertEqual(
                    archive_row["fan0_effective_write_allowed"], "true"
                )
                self.assertEqual(archive_row["runtime_home_published"], "true")
                self.assertEqual(archive_row["snapshot_mirror_configured"], "true")
                self.assertEqual(archive_row["snapshot_mirror_published"], "true")
                self.assertEqual(archive_row["status_detail"], "direct sample refreshed")
                self.assertEqual(
                    mirror_row["fan0_duty_raw"], archive_row["fan0_duty_raw"]
                )
                events = _read_runtime_events(runtime_home)
                start_events = [
                    item
                    for item in events
                    if item.get("event_type") == "read_loop.start"
                ]
                self.assertTrue(start_events, msg="read-loop start event missing")
                self.assertEqual(
                    start_events[-1]["log_csv_path"], status["log_csv_path"]
                )
                self.assertEqual(
                    start_events[-1]["event_log_path"], status["event_log_path"]
                )
                self.assertTrue(start_events[-1]["snapshot_mirror_configured"])
            finally:
                _stop_and_wait(proc)

    def test_read_loop_shutdown_updates_runtime_status(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            runtime_home = td / "runtime"
            config_path = _write_read_loop_config(
                td,
                runtime_home=runtime_home,
                poll_ms=100,
            )
            config_payload = _read_json(config_path)
            config_payload["csv_flush_interval_rows"] = 2
            _write_json(config_path, config_payload)
            proc = _spawn_control(
                ["--mode", "read-loop", "--config", str(config_path)],
                env=_sim_direct_env(channel=0, amd_temp_c=70.0),
            )
            try:
                observed = _wait_for(
                    lambda: (_read_runtime_status(runtime_home) or {}).get(
                        "successful_polls", 0
                    )
                    >= 1,
                    timeout_s=5.0,
                )
                self.assertTrue(observed)
            finally:
                code, _, stderr = _stop_and_wait(proc)

            self.assertEqual(code, 0, msg=stderr)
            final_status = _read_runtime_status(runtime_home)
            self.assertIsNotNone(final_status)
            self.assertEqual(final_status["status"], "shutdown")
            self.assertEqual(final_status["status_detail"], "stop requested")
            manifest = _read_runtime_manifest(runtime_home)
            self.assertIsNotNone(manifest)
            self.assertEqual(
                manifest["schema"], "svg_mb_control.runtime_log_manifest.v1"
            )
            self.assertEqual(manifest["status"], "completed")
            self.assertEqual(manifest["mode"], "read-loop")
            self.assertFalse(manifest["external_logging"]["required"])
            self.assertGreaterEqual(
                manifest["row_count"],
                final_status["successful_polls"],
            )
            self.assertIsNotNone(manifest["session_stop"])
            self.assertEqual(manifest["rows_written"], manifest["row_count"])
            self.assertEqual(manifest["total_rows"], manifest["row_count"])
            self.assertEqual(
                manifest["writer"]["csv_flush_policy"],
                "row_interval",
            )
            self.assertEqual(manifest["writer"]["csv_flush_interval_rows"], 2)
            self.assertEqual(
                manifest["writer"]["mirror_mode"],
                "buffered_same_interval",
            )
            self.assertTrue(Path(manifest["artifacts"]["csv_archive"]["path"]).is_file())
            self.assertTrue(Path(manifest["artifacts"]["csv_latest"]["path"]).is_file())
            self.assertEqual(
                manifest["artifacts"]["manifest_latest"]["path"],
                str(runtime_home / "logs" / "svg_mb_control_manifest.json"),
            )
            events = _read_runtime_events(runtime_home)
            self.assertEqual(manifest["event_count"], len(events))
            self.assertEqual(manifest["events_written"], len(events))
            shutdown_events = [
                item
                for item in events
                if item.get("event_type") == "read_loop.shutdown"
            ]
            self.assertTrue(shutdown_events, msg="read-loop shutdown event missing")
            self.assertEqual(
                shutdown_events[-1]["successful_polls"],
                final_status["successful_polls"],
            )
