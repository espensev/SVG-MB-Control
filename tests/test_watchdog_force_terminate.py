from __future__ import annotations

import ctypes

from tests.helpers import *

# FEAT-0008 (REQ-WATCHDOG-01/02/04): the watchdog --restart path must force-
# terminate a genuinely hung worker (one that cannot honor the graceful stop
# sentinel) and relaunch a fresh worker, rather than detecting the stall and
# leaving the loop down. The hung condition is created by SUSPENDING the real
# worker process (NtSuspendProcess) so it cannot poll the stop sentinel, with no
# test-only code in the controller. The decisive assertion is that the relaunched worker PID
# DIFFERS from the killed one: a silent relaunch no-op (old supervisor not truly
# dead -> "already running") would otherwise pass as a false recovery.

_PROCESS_SUSPEND_RESUME = 0x0800


def _nt_suspend(pid: int) -> int:
    handle = ctypes.windll.kernel32.OpenProcess(
        _PROCESS_SUSPEND_RESUME, False, pid
    )
    if not handle:
        raise OSError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
    try:
        return int(ctypes.windll.ntdll.NtSuspendProcess(handle))
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def _nt_resume(pid: int) -> None:
    handle = ctypes.windll.kernel32.OpenProcess(
        _PROCESS_SUSPEND_RESUME, False, pid
    )
    if not handle:
        return
    try:
        ctypes.windll.ntdll.NtResumeProcess(handle)
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def _pid_alive(pid: int) -> bool:
    out = StagedControlApp._run_process_filter(
        {pid},
        "exit @(Get-Process -Id $ids -ErrorAction SilentlyContinue).Count",
    )
    return out.returncode != 0


def _worker_pid(runtime_home: Path) -> int | None:
    status = _read_runtime_status(runtime_home)
    if status is None:
        return None
    pid = status.get("process_id")
    return int(pid) if pid else None


class WatchdogForceTerminateTests(WindowsExeTestCase):
    def test_hung_worker_is_force_terminated_and_relaunched(self) -> None:
        with StagedControlApp(default_mode="read-loop", poll_ms=100) as app:
            start = app.start(env=_sim_direct_env())
            self.assertEqual(start.returncode, 0, start.stderr)

            runtime_home = app.runtime_home
            old_pid = _wait_for(lambda: _worker_pid(runtime_home), timeout_s=10.0)
            self.assertTrue(old_pid, "worker did not publish a PID")

            # REQ-WATCHDOG-03: a force-kill is a crash, so the relaunched worker
            # must run ReconcilePendingWrites on startup and restore an orphaned
            # baseline. Seed a pending entry now -- the first worker already
            # passed its startup reconcile before it published a PID, so this
            # entry survives untouched until the post-force-kill relaunch.
            _seed_pending_writes(
                runtime_home,
                [
                    {
                        "channel": 0,
                        "baseline_duty_raw": 200,
                        "baseline_mode_raw": 5,
                        "target_pct": 60.0,
                        "requested_hold_ms": 0,
                        "started_iso": "2026-06-16T01:00:00",
                        "child_pid": 0,
                    }
                ],
            )

            # Suspend the worker so it can never honor the stop sentinel.
            self.assertEqual(
                _nt_suspend(old_pid), 0, "NtSuspendProcess did not succeed"
            )
            try:
                restart = app.restart(env=_sim_direct_env())
            finally:
                # Defensive: if the escalation somehow did not kill it, never
                # leave a suspended process behind for the next test.
                _nt_resume(old_pid)

            self.assertEqual(
                restart.returncode, 0,
                f"--restart did not recover:\n{restart.stdout}\n{restart.stderr}",
            )

            # The decisive check: a FRESH worker, with a different PID.
            new_pid = _wait_for(
                lambda: (_worker_pid(runtime_home)
                         if _worker_pid(runtime_home) not in (None, old_pid)
                         else None),
                timeout_s=10.0,
            )
            self.assertTrue(new_pid, "no relaunched worker PID appeared")
            self.assertNotEqual(new_pid, old_pid, "worker PID did not change")

            # The old, frozen worker is gone.
            self.assertFalse(
                _pid_alive(old_pid), "the hung worker was not terminated"
            )

            # The escalation is observable (REQ-WATCHDOG-02): the event records
            # the terminated worker PID.
            events = _read_runtime_events(runtime_home)
            killed = [
                e for e in events
                if e.get("event_type") == "supervisor.worker_force_terminated"
            ]
            self.assertTrue(
                killed, "no supervisor.worker_force_terminated event emitted"
            )
            self.assertTrue(
                any(str(old_pid) in e.get("detail", "") for e in killed),
                "force-terminate event did not record the worker PID",
            )

            # REQ-WATCHDOG-03: the relaunched worker reconciled the orphaned
            # baseline seeded before the kill (the crash-recovery path is
            # unchanged by the force-terminate escalation).
            reconciled = _wait_for(
                lambda: _read_pending_writes(runtime_home) == [],
                timeout_s=10.0,
            )
            self.assertTrue(
                reconciled,
                "force-killed relaunch did not reconcile the orphaned baseline: "
                f"{_read_pending_writes(runtime_home)}",
            )

    def test_graceful_worker_is_not_force_terminated(self) -> None:
        # REQ-WATCHDOG-04: a worker that honors the stop within the deadline
        # takes the unchanged fast path and is never force-terminated.
        with StagedControlApp(default_mode="read-loop", poll_ms=100) as app:
            start = app.start(env=_sim_direct_env())
            self.assertEqual(start.returncode, 0, start.stderr)

            runtime_home = app.runtime_home
            self.assertTrue(
                _wait_for(lambda: _worker_pid(runtime_home), timeout_s=10.0),
                "worker did not publish a PID",
            )

            restart = app.restart(env=_sim_direct_env())
            self.assertEqual(restart.returncode, 0, restart.stderr)

            events = _read_runtime_events(runtime_home)
            escalation = [
                e for e in events
                if e.get("event_type") in (
                    "supervisor.worker_force_terminated",
                    "supervisor.supervisor_force_terminated",
                    "supervisor.force_terminate_failed",
                )
            ]
            self.assertEqual(
                escalation, [],
                f"graceful restart unexpectedly escalated: {escalation}",
            )
