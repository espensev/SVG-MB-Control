# Write Orchestration

Phase 2's bounded single-channel write path. Selected with
`--mode write-once`.

## Write-once sequence

1. **Startup reconciliation.** Before any mode work, Control reads
   `runtime/pending_writes.json`. For each entry, it invokes
   `restore-auto --channel <N> --saved-duty-raw <d> --saved-mode-raw <m>`
   and waits for clean exit. Entries are removed on success. A failed
   restore blocks Control startup with a non-zero exit code.
2. **Baseline snapshot.** Control runs `svg-mb-bench.exe read-snapshot`
   and parses the resulting `current_state.json`.
3. **Baseline validation.** Control checks two preconditions on the
   target channel's fan object:
   - `snapshot_time` within `baseline_freshness_ceiling_ms` of the
     current wall clock (default 2000 ms).
   - `effective_write_allowed == true`.
   Either failure aborts the write before the sidecar is written.
4. **Sidecar upsert.** Control writes the target channel's entry to
   `pending_writes.json` via temp-file + rename.
5. **Child spawn.** Control spawns
   `svg-mb-bench.exe set-fixed-duty --channel <N> --pct <X> --hold-ms <Y>`
   through `BenchChildSupervisor` (Phase 1 module) with
   `CREATE_NEW_PROCESS_GROUP` and asynchronous pipe drainage.
6. **Wait.** Control blocks in a 50 ms polling loop until the child
   exits. If the console delivers CTRL_C or CTRL_BREAK during the wait,
   Control sends CTRL_BREAK_EVENT to the child's process group and
   waits up to 2 seconds for graceful exit.
7. **Outcome.** On child exit code 0 or signal-initiated stop, Control
   removes the sidecar entry and exits 0. On any other exit code,
   Control leaves the sidecar entry in place, prints the child's
   stderr tail, and exits with the child's exit code.

## Baseline freshness

The freshness ceiling guards against basing a write on stale fan
state. If `FanControl.exe` or LibreHardwareMonitor changed the channel
after the snapshot but before Control's write, the restore baseline
would be wrong.

Configurable via `baseline_freshness_ceiling_ms`. Default 2000 ms.

## Signal stop treated as clean exit

Set-fixed-duty's own console handler calls
`controller.restore_auto(channel)` on CTRL_BREAK, returning the channel
to motherboard auto mode. Control treats a signal-initiated stop the
same as a clean hold completion: the sidecar entry is removed. The
channel is not left in a held state.

## Reconciliation on every startup

Reconciliation runs for all modes (`one-shot`, `read-loop`,
`write-once`), not just `write-once`. This makes Control self-heal any
prior unclean exit before the user-facing mode work begins.

## Operator constraints

These are not enforced by code in Phase 2.

### Single Control instance per runtime home

The sidecar has no lock. Two concurrent Control instances sharing a
runtime home can overwrite each other's entries. Operator policy: run
at most one Control process against any given runtime home.

### Single active SIO writer per channel

If `FanControl.exe`, LibreHardwareMonitor, or another writer touches a
channel during Control's hold window, Control's baseline becomes
incorrect and restore behavior is undefined. Operator policy: ensure
no other writer is active on the target channel during a write-once
run.

Phase 2 includes a hermetic observational test
(`test_two_concurrent_control_instances_share_sidecar`) and a live
observational procedure for FanControl.exe coexistence. Neither adds
enforcement; both characterize behavior.

## Live observational procedure: FanControl.exe coexistence

Runs write-once against a known-safe channel while `FanControl.exe` is
active on the same machine. Captures:

- whether Bench's baseline snapshot matches the operator's expected
  pre-write state
- whether the channel's fan speed follows Control's target during the
  hold window, or whether FanControl.exe overrides it
- whether the channel returns to its pre-Control state after restore
- whether `FanControl.exe` reports its own state consistently across
  the sequence

This procedure is not automated. Its purpose is to characterize
real-world coexistence, not to certify it.
