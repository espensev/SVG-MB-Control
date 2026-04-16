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
entries are removed. Failed entries remain on disk and block further startup.

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
