# FEAT-0001: Hot-swap runtime write policy

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-03
**Namespace:** `REQ-WRITEPOLICY-*`
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`
**Purpose:** allow the running controller to change its write policy
(`writes_enabled`, `blocked_channels`) at a tick boundary without restarting the
process, from a single authoritative policy owner.

## 1. Summary

Today `RuntimeWritePolicy` is fixed for the lifetime of the control process: it
is read once when the `FanWriter` is built and again, unchanged, every tick when
the runtime snapshot is sampled. This feature gives the control loop a single
owned `RuntimeWritePolicy` and an operator-triggered path to change it live, so
writes can be enabled/disabled and channels blocked/unblocked on a running
controller. The operator-visible outcome is a write-policy change that takes
effect on the next tick, with any active fan override safely restored to its
captured baseline before the change applies.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, not an observed runtime failure — the
capability does not exist yet, so there is no runtime evidence of it failing. The
gap was surfaced during code review of the write-policy plumbing.

`RuntimeWritePolicy` (`src/runtime/runtime_write_policy.h:11`) is consumed in
three places that all assume it never changes after startup:

1. `SioFanWriter` pushes `writes_enabled` + `blocked_channels` into the vendored
   `MbSioController` via `init()` **once** at construction
   (`src/hardware/sio_fan_writer.cpp:89-101`). This is the hardware-enforcement
   gate; `set_fan_duty` returns `MbSioStatus::not_supported` →
   `FanWriteError::kPolicyRefused` when writes are disabled or the channel is
   blocked.
2. `SampleDirectRuntimeSnapshot` recomputes `fan.policy_blocked` and
   `fan.effective_write_allowed` from the policy **every tick** in
   `MergeFanTelemetry` (`src/platform/direct_runtime_snapshot.cpp:104-135`).
3. `TryApplyChannelSetpoint` gates each write on `effective_write_allowed` via
   `RuntimeFanAllowsWrite` (`src/control/channel_write.cpp:23-28, 297`).

These agree only because all three read the same frozen object. There is no
surface to change write policy on a running controller; the only way to disable
writes or block a channel is to restart with a different
`config/runtime_policy_*.json`. Because `MbSioController` exposes only
`init(policy)` and read-only accessors (`writes_enabled()`,
`channel_blocked()`, `write_policy()` —
`third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:117,163-165`), even a
future profile-switch path could not change the hardware gate without this work:
updating only the snapshot-side policy would leave the controller copy stale and
the two gates would disagree.

## 3. Goals & non-goals

**Goals**
- A single `RuntimeWritePolicy` owned by the control loop; the snapshot sampler
  and the `FanWriter` read from it, never from a second cached copy.
- An operator-triggered, tick-boundary path to change `writes_enabled` and
  `blocked_channels` on a running controller.
- Defined transition semantics when a change blocks a channel or disables writes
  while that channel has an active override.

**Non-goals**
- General control-profile switching (curves, deadband, cadence). That is a
  separate feature; this one covers write policy only.
- Changing `restore_on_exit` live (shutdown-time semantics; out of scope here).
- Any UI. Operator surface is the runtime-home request file + CLI, consistent
  with `docs/MEASUREMENT_GATE.md` (UI out of scope).
- Multi-threaded / mid-write policy changes. Changes apply only at tick
  boundaries.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit operator action | `AGENTS.md` §Live Runtime Safety | A change applies only from an explicit runtime-home request consumed at the tick boundary, and emits a `control_loop.write_policy_*` event for every applied or rejected change. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The needed backend change is to the in-repo vendored `third_party/SVG-MB-SIO`; no external process or sibling repo is added. |
| Shipped live-channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | *Unblocking* a channel adds a live channel and crosses the gate; it must be evidence-gated (§12). *Blocking* a channel or disabling writes only reduces authority and is always safe. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New request file and status fields are additive; absence means "no change," so existing runtime-home files stay valid. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | Write policy gates whether a computed setpoint is written; it does not change curve/blend math. No identity change. |

## 5. Behavior specification

Proposed behavior (not yet implemented):

- **Single owner, by construction.** The control loop owns one
  `RuntimeWritePolicy` instance. `SampleDirectRuntimeSnapshot` already takes it
  by `const&` (`src/platform/direct_runtime_snapshot.cpp:149-153`). On a change,
  the `FanWriter` is rebuilt from the new policy (below), so the backend copy is
  always born from the current policy — the snapshot gate and the hardware gate
  cannot drift, because there is only ever one policy, baked fresh.
- **Request intake.** A write-policy change is delivered as a runtime-home
  request file and consumed once per tick, mirroring the existing breaker-reset
  pattern (`TakeRuntimeBreakerResetRequest` /
  `ProcessCircuitBreakerResetRequest`, `src/control/tick_runner.cpp:36-128`).
  Intake happens after sampling and before the per-channel loop.
- **Apply order (build-then-swap).** The change feels like an in-place hot-swap
  to the operator but is implemented by restarting the write engine behind the
  scenes. On a valid request: (1) build a fresh `FanWriter` from the new policy
  via `CreateFanWriter(new_policy)` — if construction throws (`init`/`discover`
  failure), keep the existing writer and policy, emit a rejection event, and stop
  (the loop is never left without a working writer); (2) using the
  still-current **old** writer (its descriptors are still valid), restore the
  baseline for each channel the new policy would block, or for all `write_active`
  channels if `writes_enabled` goes false, via `RestoreSavedState(...)` and clear
  `write_active` (same path as `HandleExpiredHoldRestore`,
  `src/control/channel_write.cpp:204-221`); (3) swap in the new writer and the
  new `RuntimeWritePolicy` at the tick boundary. The next
  `SampleDirectRuntimeSnapshot` recomputes `effective_write_allowed`
  automatically. Tearing down the old writer does not auto-restore, because
  `SioFanWriter` is constructed with `restore_on_exit = false`
  (`src/hardware/sio_fan_writer.cpp:92`), so step (2) keeps the only restore
  authority.
- **Re-enable / unblock.** A channel that becomes writable again must not be
  written on the transition tick; normal control resumes on the next
  `EvaluateChannel`. The baseline is already captured, so no re-capture occurs.
- **Failure.** If building the new writer fails (step 1), the prior writer and
  policy stay in force, no partial state is left, and a rejection event is
  emitted. Because the build precedes the restore (step 2), a failed swap touches
  no channel state at all.
- **Atomicity.** The loop is single-threaded per tick; a change applies between
  ticks and never interleaves with an in-flight `ApplyDuty`.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-WRITEPOLICY-01 | The control loop must own exactly one `RuntimeWritePolicy`; the snapshot sampler must read from it and the `FanWriter` must be (re)built from it, so no second long-lived copy can drift. |
| REQ-WRITEPOLICY-02 | A policy change must be applied by building a fresh `FanWriter` from the new policy via `CreateFanWriter` (build-then-swap), reusing the existing startup construction path — not by mutating a live backend in place. No vendored `third_party/SVG-MB-SIO` API change is required. |
| REQ-WRITEPOLICY-03 | The build-then-swap must retain the previous writer and policy until the new writer is constructed successfully; a construction (`init`/`discover`) failure must leave the prior writer and policy in force and must not leave the loop without a working writer. |
| REQ-WRITEPOLICY-04 | A write-policy change must be consumed at a tick boundary, never mid-write. |
| REQ-WRITEPOLICY-05 | When a change blocks a channel or disables writes while that channel is `write_active`, the loop must restore the captured baseline and clear `write_active` before the new policy takes effect. |
| REQ-WRITEPOLICY-06 | Re-enabling writes or unblocking a channel must not issue a write on the transition tick; control resumes on the next evaluation. |
| REQ-WRITEPOLICY-07 | Unblocking a previously blocked channel must be gated against `docs/MEASUREMENT_GATE.md` (it adds a live channel). |
| REQ-WRITEPOLICY-08 | A failed backend policy push must leave the prior policy in force with no partial application, and emit a failure event. |
| REQ-WRITEPOLICY-09 | Each applied or rejected change must emit a `control_loop.write_policy_*` event recording the delta and the reason. |

## 7. Data / schema deltas

- **New runtime-home request file** (e.g. `runtime/requests/write_policy.json`):
  fields for `writes_enabled` (optional bool) and `blocked_channels` (optional
  list), modeled on the breaker-reset request file. Absence = no change.
- **New status fields** reflecting the current effective write policy, so an
  operator can confirm a change landed.
- **New event types** `control_loop.write_policy_applied` /
  `control_loop.write_policy_rejected` / `control_loop.write_policy_invalid`.
- **Schema/version impact:** additive only; update `docs/RUNTIME_HOME.md`
  (request file, status fields, events) at implementation. No existing
  runtime-home file or config becomes invalid.
- **Config impact:** none required; `config/runtime_policy_*.json` remains the
  startup default. Live changes are layered on top, not persisted by default.

## 8. CLI / config / operator surface deltas

- **New operator action** to write the request file (e.g. a subcommand that
  emits `runtime/requests/write_policy.json`), parallel to the existing breaker
  reset operator path. Respects `AGENTS.md` §Live Runtime Safety: it is an
  explicit, opt-in action.
- Update `README.md` (operator workflow) and `docs/WRITE_ORCHESTRATION.md` +
  `docs/RUNTIME_HOME.md` per `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/write-policy-hotswap-decision-2026-06-03.md`](../write-policy-hotswap-decision-2026-06-03.md) | Recreate-the-`FanWriter` (build-then-swap) **vs** in-place push-down (a new non-re-init `MbSioController` setter). Also: transition semantics for block/disable while `write_active`, and the build-then-swap failure path. | Proposed — awaits maintainer acceptance |

Leaning: **recreate via build-then-swap.** A hot-swap is a UX promise (the
operator sees a change take effect with nothing disrupted), not a requirement to
mutate a live controller. Write-policy changes are infrequent authority/safety
toggles, so per-swap re-`discover()` latency is invisible against the shipped
250 ms tick; recreate needs no vendored `third_party/SVG-MB-SIO` API change, makes
the no-drift invariant structural (the backend copy is always born from the
current policy), and reuses the startup construction path so the live and boot
behaviors cannot diverge. The transient-failure window is closed by retaining the
old writer until the new one is built (REQ-03); `restore_on_exit = false` means
teardown does not double-restore. In-place push-down would only win if policy
changes were high-frequency, which they are not.

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-WRITEPOLICY-01 | R | code review vs `docs/WRITE_ORCHESTRATION.md`: single policy owner |
| REQ-WRITEPOLICY-02 | T, R | `.\scripts\Test-LocalCI.ps1` — build-then-swap rebuilds via `CreateFanWriter`; review confirms no `third_party/SVG-MB-SIO` API change |
| REQ-WRITEPOLICY-03 | T | test: simulated `CreateFanWriter` failure on swap retains the prior writer + policy and emits a rejection event |
| REQ-WRITEPOLICY-04 | T | test: request applied at tick boundary, not mid-write |
| REQ-WRITEPOLICY-05 | T | test: block/disable while `write_active` runs restore and clears `write_active` |
| REQ-WRITEPOLICY-06 | T | test: re-enable/unblock issues no write on transition tick |
| REQ-WRITEPOLICY-07 | R, M | review vs `docs/MEASUREMENT_GATE.md`; runtime evidence when a channel is unblocked live |
| REQ-WRITEPOLICY-08 | T | test: simulated backend push failure retains prior policy, emits event |
| REQ-WRITEPOLICY-09 | T, M | test asserts events; runtime event-log evidence |

Legend: T = `Test-LocalCI` automated test; B = build/release gate; M = manual
runtime measurement (respecting Live Runtime Safety); R = code review vs the
cited contract.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Recreate (build-then-swap) vs in-place setter | implementation (§9) | Resolved-lean: recreate; no vendored setter |
| Does a live change persist to `config/runtime_policy_*.json`, or stay in-memory until restart? | implementation | In-memory only (startup config is the durable source) |
| Is per-swap re-`discover()` latency acceptable, or is a faster non-re-enumerating rebuild needed? | implementation | Acceptable — swaps are infrequent and the tick is 250 ms |

## 12. Measurement gate & dependencies

- **Measurement gate:** the *disable / block* direction reduces write authority
  and does not cross `docs/MEASUREMENT_GATE.md`. The *enable / unblock* direction
  adds a live channel and **does** cross the gate; unblocking a channel beyond
  the shipped live set requires the characterization evidence named there
  (REQ-WRITEPOLICY-07).
- **Depends on:** nothing outside this repo. The build-then-swap reuses the
  existing `CreateFanWriter` path; no `third_party/SVG-MB-SIO` API change is
  required (REQ-WRITEPOLICY-02). Per-swap re-`discover()` re-enumerates hardware;
  acceptable because swaps are infrequent.
- **Build/test impact:** new tests under `tests/`; doc updates per `AGENTS.md`
  §Change Checklist. No `CONTROL_PIPELINE_MATH.md` change (no control-math
  identity change).

## 13. Promotion-gate checklist

- [x] 1. Problem stated as a named code/contract gap with file:line evidence (§2).
- [x] 2. Stressed invariants identified — Live Runtime Safety, Repo Boundary, Measurement Gate, RUNTIME_HOME schema (§4).
- [ ] 3. Design decision record written and marked current (§9).
- [x] 4. Concrete `REQ-WRITEPOLICY-*` IDs assigned (§6).
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary; the unblock path is explicitly gated by the Measurement Gate rather than moving the baseline silently.
- [x] 7. Doctrine check: claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`.

> Gate 3 is open: this spec is `Draft` until the design decision record (§9)
> exists and is marked current. It is not buildable work yet.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-WRITEPOLICY-01 | | | |
| REQ-WRITEPOLICY-02 | | | |
| REQ-WRITEPOLICY-03 | | | |
| REQ-WRITEPOLICY-04 | | | |
| REQ-WRITEPOLICY-05 | | | |
| REQ-WRITEPOLICY-06 | | | |
| REQ-WRITEPOLICY-07 | | | |
| REQ-WRITEPOLICY-08 | | | |
| REQ-WRITEPOLICY-09 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. Update §5/§6 and the cited contract docs if behavior changes, and
bump **Updated**.>
