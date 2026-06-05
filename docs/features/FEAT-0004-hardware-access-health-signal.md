# FEAT-0004: Hardware-access dependency health signal (PawnIO availability)

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-03
**Namespace:** `REQ-HWHEALTH-*`
**Companion to:** `AGENTS.md`, `docs/STRUCTURE_AND_STABILITY.md`, `docs/RUNTIME_HOME.md`, `docs/READ_LOOP.md`, `docs/BUILD_TARGETS_AND_DEPENDENCIES.md`
**Purpose:** make PawnIO (kernel hardware-access) unavailability a distinct,
machine-readable health condition — separate from the generic `failed` /
`direct-read-failed` terminal status — so an operator or external supervisor can
act on the one failure class a process restart cannot fix.

## 1. Summary

When the PawnIO kernel interface is not available, both the AMD/SMN CPU
temperature reader and the Super I/O fan writer fail to initialize and the
controller reports a generic terminal status (`failed` or `direct-read-failed`)
that maps to health exit code `3`. The watchdog restarts only on exit code `2`
(`src/platform/task_runner.cpp:195-205`), so it never restarts on this condition
— and a restart could not help anyway, because the controller does not, and by
Repo Boundary must not, load the driver. This feature adds an additive,
machine-readable signal that names *"hardware access (PawnIO) unavailable"* as its
own observable state, distinguishing the read path (AMD/SMN) from the write path
(SIO/LPC) where the init result allows. It is observability only: it does not
load, start, or repair PawnIO, and it does not change fan-control behavior.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, confirmed by a code audit on 2026-06-03; it is
not an observed runtime failure log, because there is no distinct signal to log
today. Both hardware paths depend on PawnIO and each opens it independently with a
16-attempt retry-then-degrade sequence
(`src/hardware/amd_reader.cpp:392-493`; `third_party/SVG-MB-SIO/src/fan_sio.cpp:382-462`).

On initialization failure the two paths converge onto generic terminal status:

1. The control loop writes status `failed` when `CreateFanWriter` throws
   (`src/control/control_loop.cpp:89-107`).
2. The read loop publishes `direct-read-failed` on the same construction failure
   (`src/runtime/read_loop.cpp:184-202`).
3. `AssessHealthState` maps both `failed` and `direct-read-failed` to `kFailed`
   (`src/runtime/runtime_health.cpp:111-116`), and `RuntimeHealthExitCode` maps
   `kFailed` → exit code `3` (`src/runtime/runtime_health.cpp:219-232`).
4. The watchdog restarts only on exit code `2` and returns `3` unchanged — no
   action (`src/platform/task_runner.cpp:195-205`).

`AssessHealthState` evaluates status string, process liveness, and staleness only
(`src/runtime/runtime_health.cpp:75-177`); nothing validates whether the PawnIO
device is reachable. Once init fails there is no per-tick reconnect: `Sample()`
returns `available = false` indefinitely
(`src/hardware/amd_reader.cpp:561-672`, esp. `:579-582`), carrying only a
human-readable string in `last_warning` (`src/hardware/amd_reader.cpp:553-559`).

Consequence: the failure class most able to take the whole controller offline
with no self-recovery — PawnIO not loaded at boot — is, at the health surface,
indistinguishable from any other terminal failure, and is routed to an exit code
the watchdog ignores. There is no first-class signal an operator or external
recovery agent can subscribe to in order to perform the only effective recovery
(reload the driver, then restart the controller).

## 3. Goals & non-goals

**Goals**
- Add a distinct, machine-readable signal that identifies hardware-access
  (PawnIO) unavailability separately from other terminal failures.
- Distinguish read-path (AMD/SMN) from write-path (SIO/LPC) availability where
  the initialization results allow, since the audit confirmed the two paths can
  fail independently.
- Make the signal observable to an operator and an external supervisor (status
  field + transition event + `--diagnose` / `--health` reporting), so the
  effective recovery can be performed by whatever is actually able to do it.

**Non-goals**
- The controller must not load, start, or restart the PawnIO driver itself
  (`AGENTS.md` §Repo Boundary; the audit confirmed no such code exists and a
  process restart cannot create the driver). Recovery action stays external.
- No change to fan-control computation, cadence, channels, or write behavior.
- No new per-tick PawnIO reconnect loop. Self-healing the connection is a
  separate reliability feature; this one surfaces the condition (§11).
- No change to the watchdog restart policy in this feature. Whether and how
  anything should act on the new signal is a downstream decision (§9, §11).

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Adds only an in-repo health/status surface fed by existing init results; introduces no driver-management code and no external process. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The feature is read-only observability; it must not write fan duty, start/stop, or reset breakers. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New status field(s) and event type(s) are additive; absence means "unknown," and the existing `failed` / `direct-read-failed` values keep their current meaning. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | No curve, blend, cadence, boost, or write-gate change. No identity change. |
| Shipped cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | Read-only telemetry; does not change cadence, channels, or input strategy. Does not cross the gate. |

## 5. Behavior specification

Proposed behavior (not yet implemented):

- **Distinct availability indicator.** Define a hardware-access availability
  signal with at least: an overall state, and per-path read (AMD/SMN) and write
  (SIO/LPC) availability where the init result distinguishes them. It is
  populated from the existing initialization outcomes: AMD reader availability
  and `last_warning` (`src/hardware/amd_reader.cpp:553-559, 579-582`) and the SIO
  writer construction outcome caught at `src/runtime/read_loop.cpp:184-202`
  (thrown from `src/hardware/sio_fan_writer.cpp:87-108`).
- **No false-positive availability.** The signal reports *unknown / unavailable*,
  not *healthy*, when no successful PawnIO open has occurred — mirroring the
  "no false zero" rule in `FEAT-0002`. Availability is asserted only on an
  observed successful open.
- **Surfaced two ways.** (a) An additive status field in the runtime status /
  sidecar so an operator can read the current state, and (b) a transition event
  (e.g. `control_loop.hwaccess_unavailable` / `control_loop.hwaccess_restored`)
  recording which path(s) and the underlying warning text.
- **Backward-compatible.** The existing terminal status values `failed` and
  `direct-read-failed` are retained with unchanged meaning; the new signal is a
  more specific, additive overlay, not a rename.
- **Exit-code mapping unchanged in this feature.** Whether the new signal should
  change the health exit-code mapping (and therefore the watchdog contract) is a
  direction-setting decision (§9). The default for this feature is additive
  status + event with exit codes unchanged, because changing exit-code semantics
  alters the `task_runner` watchdog contract and deserves its own decision.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-HWHEALTH-01 | The runtime status surface must expose a hardware-access availability signal distinct from the generic `failed` / `direct-read-failed` status values, set from the AMD reader and SIO writer initialization outcomes. |
| REQ-HWHEALTH-02 | The signal must distinguish read-path (AMD/SMN) from write-path (SIO/LPC) availability when the initialization results allow that distinction. |
| REQ-HWHEALTH-03 | The signal must be additive to the runtime-home / status schema; existing status files, archives, and the existing `failed` / `direct-read-failed` values must remain valid and unchanged in meaning. |
| REQ-HWHEALTH-04 | A transition into or out of hardware-access-unavailable must emit a corresponding event recording the affected path(s) and the underlying warning text. |
| REQ-HWHEALTH-05 | The controller must not attempt to load, start, or restart the PawnIO driver; the feature is observability only (`AGENTS.md` §Repo Boundary / §Live Runtime Safety). |
| REQ-HWHEALTH-06 | The signal must report unknown/unavailable rather than healthy when no successful PawnIO open has occurred (no false-positive availability). |

## 7. Data / schema deltas

- **New status fields** (additive, tri-state — available / unavailable /
  unknown), e.g. `hwaccess_read_available`, `hwaccess_write_available`, so an
  operator can confirm which path is down.
- **New event types** `control_loop.hwaccess_unavailable` /
  `control_loop.hwaccess_restored`, carrying path and warning text.
- **Schema/version impact:** additive only; update `docs/RUNTIME_HOME.md` (status
  fields, events) at implementation. No existing runtime-home file, archive, or
  config becomes invalid.
- **Config impact:** none.

## 8. CLI / config / operator surface deltas

- Extend `--diagnose` / `--health` reporting (`src/app/app_diagnose.cpp`,
  `src/app/app_args.cpp`) to include the hardware-access signal, read-only.
- Update `README.md` (operator workflow), `docs/RUNTIME_HOME.md`, and
  `docs/BUILD_TARGETS_AND_DEPENDENCIES.md` §Drivers and Hardware Access per
  `AGENTS.md` §Change Checklist when the surface changes. UI is out of scope
  (`docs/MEASUREMENT_GATE.md`).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/hwaccess-health-signal-decision-YYYY-MM-DD.md` | Whether the hardware-access-unavailable signal changes the health exit-code mapping (and thus the `task_runner` watchdog contract) or stays additive status/event only; whether read-path and write-path each get their own exit code or only an informational field; and whether a future external recovery agent or the watchdog should act on it. | Needed |

Leaning: **additive status + event, exit codes unchanged in this feature.** The
audit shows a restart cannot fix a PawnIO-absent condition, so re-routing it to
the restart-on-`2` path would only induce flapping. The value is making the
condition *nameable and observable* so the effective fix (driver reload, then
restart) can be triggered by an operator or an external supervisor. Changing the
watchdog's restart contract is a separate decision that should not be bundled
into the observability change.

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-HWHEALTH-01 | T, R | `.\scripts\Test-LocalCI.ps1` status-field test; review vs `docs/RUNTIME_HOME.md` |
| REQ-HWHEALTH-02 | T | test: read-path-down vs write-path-down init outcomes set distinct fields |
| REQ-HWHEALTH-03 | T, R | analyzer/ingest test with old status files missing the new fields; review for additive-only schema |
| REQ-HWHEALTH-04 | T, M | test asserts transition events; runtime event-log evidence |
| REQ-HWHEALTH-05 | R | code review: no driver load/start/restart path added |
| REQ-HWHEALTH-06 | T | test: no successful open ⇒ signal is unknown/unavailable, never healthy |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Change exit-code mapping vs additive status/event only | implementation (§9) | Additive only this feature; exit codes unchanged |
| Add a per-tick PawnIO reconnect attempt | a future reliability feature | Out of scope here; surface the condition only |
| Should the watchdog or an external agent act on the signal, and with what action | a future decision | Out of scope; surface only |
| One overall field vs separate read/write fields | implementation | Separate read/write fields (paths fail independently) |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change fan channels, cadence, write timing, or
  control math; read-only evidence. Does not cross `docs/MEASUREMENT_GATE.md`.
- **Depends on:** existing initialization/status surfaces
  (`amd_reader`, `sio_fan_writer`/`read_loop`, `runtime_health`,
  `runtime_status`). Related to `FEAT-0005`, which can consume the same signal.
- **Build/test impact:** status-schema compatibility tests under `tests/`; doc
  updates per `AGENTS.md` §Change Checklist. No `CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist

- [x] 1. Problem stated as a named code/contract gap with file:line evidence (§2).
- [x] 2. Stressed invariants identified — Repo Boundary, Live Runtime Safety, RUNTIME_HOME schema, control identity, Measurement Gate (§4).
- [ ] 3. Required design decision record written and marked current (§9).
- [x] 4. Concrete `REQ-HWHEALTH-*` IDs assigned (§6).
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary and does not move the Measurement Gate baseline (read-only; no driver management).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`.

> Gate 3 is open: this spec is `Draft` until the design decision record (§9)
> exists and is marked current. It is not buildable work yet.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / status / note) | Checked (date) |
|---|---|---|---|
| REQ-HWHEALTH-01 | | | |
| REQ-HWHEALTH-02 | | | |
| REQ-HWHEALTH-03 | | | |
| REQ-HWHEALTH-04 | | | |
| REQ-HWHEALTH-05 | | | |
| REQ-HWHEALTH-06 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. Update §5/§6 and the cited contract docs if behavior changes, and
bump **Updated**.>
