# Write-Policy Hot-Swap Decision - 2026-06-03

Status: Accepted (maintainer, 2026-06-06) — current design decision for
`docs/features/FEAT-0001-hot-swap-write-policy.md`. Not yet implemented;
normative for FEAT-0001 implementation.

**Companion to:** `docs/features/FEAT-0001-hot-swap-write-policy.md`,
`docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`.

## Problem

`RuntimeWritePolicy` (`src/runtime/runtime_write_policy.h:11`) is fixed for the
lifetime of the control process. It is consumed in three places that all assume
it never changes after startup:

1. `SioFanWriter` pushes `writes_enabled` + `blocked_channels` into the vendored
   `MbSioController` via `init()` **once** at construction
   (`src/hardware/sio_fan_writer.cpp:89-101`). This is the hardware-enforcement
   gate (`set_fan_duty` returns `MbSioStatus::not_supported` →
   `FanWriteError::kPolicyRefused`).
2. `SampleDirectRuntimeSnapshot` recomputes `fan.policy_blocked` and
   `fan.effective_write_allowed` from the policy **every tick**
   (`src/platform/direct_runtime_snapshot.cpp:104-135`).
3. `TryApplyChannelSetpoint` gates each write on `effective_write_allowed`
   (`src/control/channel_write.cpp:23-28, 297`).

FEAT-0001 lets an operator change `writes_enabled` / `blocked_channels` on a
running controller. This decision settles **how the change reaches the hardware
gate (#1)**, since the snapshot gate (#2/#3) already re-reads the policy each tick
and only needs to be pointed at a single owned policy object.

The hard constraint: `MbSioController` exposes only `init(policy)` plus read-only
accessors (`writes_enabled()`, `channel_blocked()`, `write_policy()` —
`third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:117,163-165`). There is no
in-place policy setter today.

## Options considered

### Option A — Recreate the `FanWriter` (build-then-swap)  *(chosen)*

On a change, build a fresh writer from the new policy via
`CreateFanWriter(new_policy)`, then swap it in. The backend copy is reconstructed
from the new policy via the existing startup path.

- No `third_party/SVG-MB-SIO` change.
- No-drift is structural: the backend copy is always born from the current policy.
- Live-change path is the same code as startup (`CreateFanWriter → discover`), so
  the two cannot diverge.
- Cost: each swap re-runs `init()`/`discover()` (re-enumerates hardware; the
  descriptor generation counter increments and prior descriptors go stale, per
  `svg_mb_sio.h:124`).
- Risk: `init()` can throw; mitigated by build-then-swap (retain old writer until
  the new one is built).

### Option B — In-place push-down (new `MbSioController` setter)

Add a non-re-init `set_write_policy()` to the vendored controller and an
`UpdateWritePolicy()` on `FanWriter` that calls it.

- No re-enumeration; cheapest per change.
- Requires editing vendored `third_party/SVG-MB-SIO` and defining its atomicity
  vs an in-flight `set_fan_duty`.
- Keeps two policy copies (controller + loop) that must be kept in lockstep — the
  drift risk becomes an enforced invariant instead of a structural one.

### Option C — Snapshot-gate only (no backend change)

Update only the loop's policy feeding `SampleDirectRuntimeSnapshot`, leaving the
controller's copy untouched.

- Rejected: the two gates would disagree. Re-enabling writes via the snapshot
  gate would pass `RuntimeFanAllowsWrite` and then be refused by the controller
  (`kPolicyRefused`), because the controller was initialized disabled. A blocked
  channel unblocked this way would never actually write.

## Decision

Adopt **Option A — recreate the `FanWriter` via build-then-swap**, applied at the
tick boundary.

Rationale:

- A hot-swap is a UX promise — the operator sees a change take effect with nothing
  disrupted — not a requirement to mutate a live controller. Restarting the write
  engine behind the scenes satisfies that promise.
- Write-policy changes are infrequent authority/safety toggles (operator action,
  profile switch), not a per-tick signal. Per-swap re-`discover()` latency is
  invisible against the shipped 250 ms tick (`docs/MEASUREMENT_GATE.md`).
- It needs no vendored API change and reuses the construction path that already
  ships, so the live and boot behaviors cannot diverge.

Option B would only win if policy changes were high-frequency, which they are not.
If that assumption ever changes (write policy driven by a fast signal), revisit B.

## Apply order (normative for implementation)

A change is delivered as a runtime-home request file consumed once per tick,
mirroring the breaker-reset pattern (`TakeRuntimeBreakerResetRequest` /
`ProcessCircuitBreakerResetRequest`, `src/control/tick_runner.cpp:36-128`). On a
valid request:

1. **Build.** Construct the new writer with `CreateFanWriter(new_policy)`. If it
   throws (`init`/`discover` failure), keep the existing writer and policy, emit
   `control_loop.write_policy_rejected`, and stop. No channel state is touched.
2. **Restore with the old writer.** Using the still-current writer (its
   descriptors are still valid), restore the captured baseline for each channel
   the new policy would block, and for all `write_active` channels if
   `writes_enabled` goes false, via `RestoreSavedState(...)`, then clear
   `write_active` (same path as `HandleExpiredHoldRestore`,
   `src/control/channel_write.cpp:204-221`).
3. **Swap.** Replace the writer and the loop's `RuntimeWritePolicy` at the tick
   boundary, then emit `control_loop.write_policy_applied` with the delta. The
   next `SampleDirectRuntimeSnapshot` recomputes `effective_write_allowed`.

Re-enabling / unblocking issues no write on the transition tick; normal control
resumes on the next `EvaluateChannel`.

## Consequences

- One owned `RuntimeWritePolicy` becomes the single source of truth; the snapshot
  sampler reads it and the writer is rebuilt from it (FEAT-0001 REQ-WRITEPOLICY-01).
- Tearing down the old writer does **not** auto-restore, because `SioFanWriter`
  constructs with `restore_on_exit = false` (`src/hardware/sio_fan_writer.cpp:92`);
  step 2 keeps the only restore authority, so there is no double-restore.
- Each swap re-enumerates hardware. Acceptable at the expected change frequency;
  noted as an open item in FEAT-0001 §11 if a non-re-enumerating rebuild is later
  needed.
- The *unblock* direction adds a live channel and crosses
  `docs/MEASUREMENT_GATE.md`; it must be evidence-gated (FEAT-0001
  REQ-WRITEPOLICY-07). The *block / disable* direction only reduces authority and
  is always safe.
- The simulated backend (`MakeSimulatedFanWriter`) is rebuilt the same way, so
  tests exercise build-then-swap without hardware.
- No control-computation identity change; `docs/CONTROL_PIPELINE_MATH.md` is
  unaffected.

## Verification

- `.\scripts\Test-LocalCI.ps1`: build-then-swap rebuilds via `CreateFanWriter`; a
  simulated construction failure on swap retains the prior writer + policy and
  emits a rejection; block/disable while `write_active` restores baseline and
  clears `write_active`; re-enable/unblock issues no write on the transition tick.
- Code review vs `docs/WRITE_ORCHESTRATION.md` and this decision: single policy
  owner, no `third_party/SVG-MB-SIO` API change.
- Runtime event-log evidence for `control_loop.write_policy_*` when exercised live
  (respecting `AGENTS.md` §Live Runtime Safety).
