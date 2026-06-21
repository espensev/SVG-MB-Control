from __future__ import annotations

from tests.helpers import *

# FEAT-0023 (REQ-MPROFILE-04/05/06): the supervisor consumes a profile-switch
# request, validates the candidate, and (for a valid one) gracefully cycles the
# worker into the new profile WITHOUT crash backoff. A rejected request must
# leave the running worker untouched. These tests drive a real supervised app in
# sim mode and assert on durable artifacts (events, PID identity, restart_count),
# not on timing.
#
# The auto-revert wiring (REQ-MPROFILE-07) is reachable only by a profile that
# parse-validates at the supervisor but fails the worker's hardware bind at
# startup; there is no sim hook for that, so revert is covered by the
# profile_switch_decision unit tests (DecideAfterStartupOutcome) plus live
# validation, not here.


def _worker_pid(runtime_home: Path) -> int | None:
    status = _read_runtime_status(runtime_home)
    if not status:
        return None
    pid = status.get("process_id")
    return int(pid) if pid else None


def _supervisor_restart_count(runtime_home: Path) -> int | None:
    path = runtime_home / "control_supervisor.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8")).get(
        "worker_restart_count"
    )


def _active_profile(runtime_home: Path) -> str | None:
    path = runtime_home / "control_supervisor.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8")).get("active_profile_name")


def _worker_profile(runtime_home: Path) -> tuple[str | None, str | None]:
    """(active_profile_name, active_profile_source) from the worker status."""
    status = _read_runtime_status(runtime_home)
    if not status:
        return (None, None)
    return (status.get("active_profile_name"), status.get("active_profile_source"))


def _event_types(runtime_home: Path) -> list[str]:
    return [e.get("event_type") for e in _read_runtime_events(runtime_home)]


def _write_profile(root: Path, runtime_home: Path, name: str, poll_ms: int) -> None:
    profiles = root / "config" / "profiles"
    profiles.mkdir(parents=True, exist_ok=True)
    cfg = {
        "schema_version": 4,
        "runtime_home_path": runtime_home.as_posix(),
        "poll_ms": poll_ms,
        "baseline_freshness_ceiling_ms": 10000,
        "restore_timeout_ms": 5000,
        "default_mode": "read-loop",
    }
    (profiles / f"{name}.json").write_text(
        json.dumps(cfg, indent=2), encoding="utf-8"
    )


def _seed_switch_request(runtime_home: Path, profile: str) -> None:
    runtime_home.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "requested_at": "2026-06-21T00:00:00",
        "reason": "operator-request",
        "profile": profile,
    }
    (runtime_home / "profile.switch.request.json").write_text(
        json.dumps(payload, indent=2), encoding="utf-8"
    )


def _switch_into_new_worker(app: StagedControlApp, profile: str,
                            old_pid: int) -> int | None:
    _seed_switch_request(app.runtime_home, profile)
    return _wait_for(
        lambda: (_worker_pid(app.runtime_home)
                 if _worker_pid(app.runtime_home) not in (None, old_pid)
                 else None),
        timeout_s=25.0,
    )


class ProfileSwitchTests(WindowsExeTestCase):
    def test_unknown_profile_switch_is_rejected_and_worker_untouched(self) -> None:
        with StagedControlApp(default_mode="read-loop", poll_ms=100) as app:
            self.assertEqual(app.start(env=_sim_direct_env()).returncode, 0)
            runtime_home = app.runtime_home
            pid = _wait_for(lambda: _worker_pid(runtime_home), timeout_s=12.0)
            self.assertTrue(pid, "worker did not publish a PID")

            _seed_switch_request(runtime_home, "does-not-exist")

            rejected = _wait_for(
                lambda: "supervisor.profile_rejected" in _event_types(runtime_home),
                timeout_s=12.0,
            )
            self.assertTrue(rejected, "no supervisor.profile_rejected event")
            # The healthy worker must not be disrupted by a rejected switch.
            self.assertEqual(_worker_pid(runtime_home), pid,
                             "a rejected switch disrupted the running worker")
            # The request is consumed (take-once), so the file is gone.
            self.assertFalse(
                (runtime_home / "profile.switch.request.json").exists(),
                "the rejected switch request was not cleared",
            )

    def test_valid_profile_switch_cycles_worker_without_backoff(self) -> None:
        with StagedControlApp(default_mode="read-loop", poll_ms=100) as app:
            runtime_home = app.runtime_home
            _write_profile(app.root, runtime_home, "alt", poll_ms=150)
            self.assertEqual(app.start(env=_sim_direct_env()).returncode, 0)

            old_pid = _wait_for(lambda: _worker_pid(runtime_home), timeout_s=12.0)
            self.assertTrue(old_pid, "worker did not publish a PID")
            restart_before = _supervisor_restart_count(runtime_home)

            new_pid = _switch_into_new_worker(app, "alt", old_pid)
            self.assertTrue(new_pid and new_pid != old_pid,
                            "the worker did not cycle into the new profile")

            events = _event_types(runtime_home)
            self.assertIn("supervisor.profile_applied", events,
                          "no supervisor.profile_applied event")
            # No crash backoff: the switch must use the no-backoff respawn path.
            self.assertNotIn(
                "supervisor.worker_restart_scheduled", events,
                "the switch incurred crash backoff (restart scheduled)",
            )
            self.assertEqual(
                _supervisor_restart_count(runtime_home), restart_before,
                "the switch bumped worker_restart_count",
            )
            # REQ-MPROFILE-09: the active profile is recorded in runtime status.
            self.assertTrue(
                _wait_for(lambda: _active_profile(runtime_home) == "alt",
                          timeout_s=10.0),
                "control_supervisor.json active_profile_name did not flip to the "
                "switched profile",
            )
            # REQ-MPROFILE-09: name AND resolution source land in the WORKER
            # runtime status (control_runtime.json), not only the supervisor
            # state. The switched worker runs --config .../profiles/alt.json, so
            # its profile name is the config stem and the source is the worker's
            # own resolution (explicit config path).
            self.assertTrue(
                _wait_for(
                    lambda: _worker_profile(runtime_home)[0] == "alt",
                    timeout_s=10.0),
                "control_runtime.json active_profile_name did not record the "
                "switched profile",
            )
            self.assertEqual(
                _worker_profile(runtime_home)[1], "explicit_config",
                "control_runtime.json active_profile_source not recorded",
            )

    def test_two_consecutive_switches(self) -> None:
        with StagedControlApp(default_mode="read-loop", poll_ms=100) as app:
            runtime_home = app.runtime_home
            _write_profile(app.root, runtime_home, "alt", poll_ms=150)
            _write_profile(app.root, runtime_home, "alt2", poll_ms=170)
            self.assertEqual(app.start(env=_sim_direct_env()).returncode, 0)

            pid0 = _wait_for(lambda: _worker_pid(runtime_home), timeout_s=12.0)
            self.assertTrue(pid0, "worker did not publish a PID")

            pid1 = _switch_into_new_worker(app, "alt", pid0)
            self.assertTrue(pid1 and pid1 != pid0, "first switch did not cycle")

            # A second switch right after the first proves the cycle signal was
            # cleared (a stale signal would make the new worker instantly exit).
            pid2 = _switch_into_new_worker(app, "alt2", pid1)
            self.assertTrue(
                pid2 and pid2 != pid1,
                "second switch did not cycle (stale cycle signal?)",
            )
