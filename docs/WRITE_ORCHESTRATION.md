# Write Orchestration

## Purpose

`write-once` is the bounded direct write path. It captures a fresh baseline,
applies one duty override, holds for a bounded window, then restores the
baseline.

## Inputs

Top-level config fields used by `write-once`:

- `runtime_home_path`
- `baseline_freshness_ceiling_ms`
- `restore_timeout_ms`
- `runtime_policy_path`
- optional `write_channel`
- optional `write_target_pct`
- optional `write_hold_ms`

CLI can override the write fields with:

- `--write-channel`
- `--write-pct`
- `--write-hold-ms`

## Runtime Flow

1. Reconcile any existing `pending_writes.json` entries before mode dispatch.
2. Initialize the direct fan backend.
3. Sample a fresh direct runtime snapshot.
4. Capture the baseline duty and mode for the target channel.
5. Reject stale snapshots or policy-blocked targets.
6. Write a pending-write sidecar entry before touching hardware.
7. Apply the requested duty directly.
8. Hold until timeout or stop request.
9. Restore the baseline duty and mode.
10. Remove the pending-write sidecar entry.

## Reconciliation

Startup reconciliation reads `runtime\pending_writes.json` and attempts to
restore each stored baseline directly through Control's own writer. Successful
entries are removed. Failed restores remain on disk and block further startup.

A **corrupt or unparseable** `pending_writes.json` is not fatal (FEAT-0012): the
startup read quarantines the bad file to `pending_writes.json.corrupt` (preserving
the original bytes), emits `reconcile.sidecar_quarantined`, degrades runtime health
(`sidecar_quarantined_present`, surfaced in the `--health` JSON), and proceeds as if
the sidecar were empty — so a corrupt recovery file cannot trap the worker in a
relaunch loop. The control loop then reasserts authority through its normal startup
path. The parsed-but-failed-restore case above is unchanged.

## Control-loop sidecar persist cadence (FEAT-0019)

In the control loop, the sidecar is persisted synchronously before `ApplyDuty`
only on a recovery-relevant identity change — the first write that activates a
channel (entry created, baseline captured) or a baseline re-capture. Same-baseline
setpoint churn during a ramp marks the store dirty and is written by the existing
once-per-tick end-of-tick `Flush()` rather than persisted synchronously, so no
fsync'd atomic file-replace runs before `ApplyDuty` during a ramp. The
crash-recovery guarantee is unchanged because the recovery-relevant baseline is
recorded synchronously at activation and recovery never reads the deferred
`target_pct` (see `docs/RUNTIME_HOME.md`). A successful `Flush()` rewrites the whole
sidecar, so any persist-failure health degradation a deferred write self-heals is
then cleared.

## Control-loop sidecar persist failure (FEAT-0010)

In the control loop (`TryApplyChannelSetpoint`) the sidecar upsert (Runtime Flow
step 6) precedes the fan write so a crash mid-write leaves a recovery record. A
*failure* to persist that sidecar entry does **not** veto the fan write: the
controller logs `control_loop.sidecar_upsert_failed`, increments the additive
per-channel `consecutive_sidecar_persist_failures` counter (which degrades
health), and still applies the computed duty — including the sensor-safe 100%
command. `PendingWritesStore::Upsert` updates its in-memory entry before the
throw, so the next successful tick re-persists; the captured baseline
(`baseline_duty_raw`/`baseline_mode_raw`) is stable across ticks, so a
stale-but-present entry still restores correctly on reconcile. The write-failure
breaker and `consecutive_write_failures` are untouched because the actuation
itself did not fail. (A first-write-with-failed-persist crash leaves no entry for
that channel; the in-window command is a cooling command and the next worker
re-establishes control — an accepted residual.)

## Logging

`write-once` and startup reconciliation append durable events to
`runtime\logs\svg_mb_control_events.jsonl` for validation failures, policy
refusals, applied writes, restores, reconcile work, and failures.

They do not create a dedicated CSV telemetry chunk; the event log is the write
orchestration trace surface for this tier.

## Exit Behavior

- Policy refusal before a write returns exit code `2` and clears the sidecar.
- Write or restore failures return non-zero and leave any unresolved sidecar
  state in place.
- `Ctrl+C` / `Ctrl+Break` during the hold window triggers restore and normal
  sidecar cleanup.

## Constraints

- Writes are owned here; they are not delegated to another executable.
- New feature work must keep baseline capture, sidecar ownership, write, and
  restore inside this repo.
- Any broader write-cadence or fan-response retuning should follow the
  prerequisite measurement work in `docs\MEASUREMENT_GATE.md`.
