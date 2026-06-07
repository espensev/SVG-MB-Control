# svg-mb-control - Traceability

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.1   **Updated:** 2026-06-07
**Companion to:** `AGENTS.md`, `docs/features/README.md`
**Purpose:** central `REQ-*` to verification map for feature specs.

## 1. How to use this file

This file is the project-wide audit surface for feature requirements. The owning
`docs/features/FEAT-*.md` spec remains the source of truth for behavior,
acceptance criteria, open decisions, and verification logs; this file maps each
`REQ-*` ID to its verification home and current result.

When a feature requirement is added, removed, renamed, split, or implemented,
update this file in the same change as the owning feature spec. A requirement
without a row here is not ready for implementation review.

The structural parts of this contract are checked by
`tests/test_feature_specs.py`, which runs in the Python test lane. That test
validates feature registry rows, `REQ-*` coverage in spec sections 6/10/14,
traceability rows, promotion-gate state for accepted/implemented specs, and
implemented verification-log results.

That test checks that each requirement has a row here and a valid verify code,
not that the cited check exists or passes: a green lane means the spec and
traceability structure are consistent, not that every requirement is
independently re-verified.

Verify legend:

- **T** = automated test through `.\scripts\Test-LocalCI.ps1`.
- **B** = build/release gate through `.\build-release.ps1` or
  `scripts\Build-Release.ps1`.
- **M** = manual runtime measurement or evidence, respecting `AGENTS.md` Live
  Runtime Safety.
- **R** = code review against the cited contract, decision record, or source.

Result values:

- **pending** = not implemented or not yet verified.
- **pass** = implemented and verified in the owning spec's verification log.
- **deferred** = explicitly split out of the implemented slice.
- **not buildable** = the spec has open promotion gates.

## 2. Feature status

| Feature | Status | Buildability |
|---|---|---|
| `FEAT-0001` Hot-swap runtime write policy | Accepted | Buildable when implementation is explicitly authorized; verification pending. |
| `FEAT-0002` CPU settings evidence logger | Implemented (source/test load layer; live package verification pending; label deferred) | Source/test requirements pass except `REQ-CPUSETTINGS-06`, which is deferred; the active `release\` package inspected on 2026-06-07 was stale and needs rebuilt CSV-header confirmation before live evidence counts for FEAT-0002. |
| `FEAT-0003` Selectable control-law profile with hot-swap | Draft | Not buildable; design capture only. |
| `FEAT-0004` Hardware-access dependency health signal | Draft | Not buildable; decision record gate open. |
| `FEAT-0005` Write actuation confirmation | Reserved (body parked) | Not buildable; body parked in `docs/features/_parked/`, sequenced behind FEAT-0004. |
| `FEAT-0006` CPU work and energy efficiency evidence | Draft | Not buildable; promoted to Draft 2026-06-07 with all promotion gates met. Implementation gated on a one-shot read-only live MSR validation (RAPL on Family 1Ah; PawnIO affinity; `#GP`→blank). FEAT-0004 recommended, not blocking. |
| `FEAT-0007` RAM temperature telemetry | Reserved (body parked) | Not buildable; body parked. Read path exists (SVG-MB-SIO `read_sio_temperatures` DIMM sources); promotion would require DIMM-source validity confirmation from `evidence-log` plus a sampling/schema decision. |

## 3. Requirement map

### FEAT-0001 - Hot-swap runtime write policy

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-WRITEPOLICY-01` | R | Review `WRITE_ORCHESTRATION.md`: single policy owner. | pending |
| `REQ-WRITEPOLICY-02` | T, R | Test build-then-swap rebuilds through `CreateFanWriter`; review no `third_party/SVG-MB-SIO` API change. | pending |
| `REQ-WRITEPOLICY-03` | T | Simulated `CreateFanWriter` failure retains prior writer and policy and emits rejection. | pending |
| `REQ-WRITEPOLICY-04` | T | Request applies at tick boundary, not mid-write. | pending |
| `REQ-WRITEPOLICY-05` | T | Block/disable while `write_active` restores baseline and clears `write_active`. | pending |
| `REQ-WRITEPOLICY-06` | T | Re-enable/unblock issues no write on the transition tick. | pending |
| `REQ-WRITEPOLICY-07` | R, M | Review `MEASUREMENT_GATE.md`; runtime evidence before any live channel unblock. | pending |
| `REQ-WRITEPOLICY-08` | T | Simulated backend policy-push failure retains prior policy and emits event. | pending |
| `REQ-WRITEPOLICY-09` | T, M | Tests assert events; runtime event-log evidence for applied/rejected changes. | pending |

### FEAT-0002 - CPU settings evidence logger

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-CPUSETTINGS-01` | T, R | CSV/header compatibility tests and `RUNTIME_HOME.md` review. | pass |
| `REQ-CPUSETTINGS-02` | T | System CPU delta calculation unit/smoke test; live package header recheck pending after rebuild. | pass |
| `REQ-CPUSETTINGS-03` | T, R | Analyzer ingest compatibility with old archives missing new fields. | pass |
| `REQ-CPUSETTINGS-04` | R | Review confirms Win32 first-party source only; no tool/subprocess/sibling dependency. | pass |
| `REQ-CPUSETTINGS-05` | R | Review confirms logger records raw values only, with no activity classification. | pass |
| `REQ-CPUSETTINGS-06` | T, R | Optional CPU-settings label propagation test/config review. | deferred |

### FEAT-0003 - Selectable control-law profile with hot-swap

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-PROFILE-01` | T, R | Curve-overlay output-equivalence test vs current `EvaluateChannel`; review single call site. | not buildable |
| `REQ-PROFILE-02` | T | One `PidController` covers P, PI, PD, and PID by gain selection. | not buildable |
| `REQ-PROFILE-03` | T, R | Config-load tests for absent `controller` and per-law validation. | not buildable |
| `REQ-PROFILE-04` | T | Profile request applies at tick boundary; validation failure retains running profile and emits rejection. | not buildable |
| `REQ-PROFILE-05` | T | Swap resets new controller dynamic state by default. | not buildable |
| `REQ-PROFILE-06` | T, R | Sensor-safe mode, deadband, cooldown, breaker, clamp, and write gates behave identically by controller kind. | not buildable |
| `REQ-PROFILE-07` | T, R, M | Config-load test that `pid.allow_live: true` is rejected without characterization evidence and a non-NaN slew cap; review vs. `MEASUREMENT_GATE.md` and decision record D6; PID runs shadow/dry-run by default, live only under an evidenced and slew-bounded `allow_live` crossing. | not buildable |
| `REQ-PROFILE-08` | T | CSV/status tests assert per-channel controller-kind field and kind-aware/nullable law-specific reporting fields. | not buildable |
| `REQ-PROFILE-09` | T, R | Differing channel-set candidate rejected until FEAT-0001 restore/capture path exists. | not buildable |
| `REQ-PROFILE-10` | R | Review control-identity docs for curve-overlay scope and PID identity reference. | not buildable |

### FEAT-0004 - Hardware-access dependency health signal

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-HWHEALTH-01` | T, R | Status-field test; review `RUNTIME_HOME.md`. | not buildable |
| `REQ-HWHEALTH-02` | T | Read-path-down vs write-path-down init outcomes set distinct fields. | not buildable |
| `REQ-HWHEALTH-03` | T, R | Analyzer/ingest compatibility with old status files; additive-only schema review. | not buildable |
| `REQ-HWHEALTH-04` | T, M | Tests assert transition events; runtime event-log evidence. | not buildable |
| `REQ-HWHEALTH-05` | R | Review confirms no driver load/start/restart path. | not buildable |
| `REQ-HWHEALTH-06` | T | No successful open means unknown/unavailable, never healthy. | not buildable |

### FEAT-0006 - CPU work and energy efficiency evidence

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-CPUEFF-01` | T, M | Work-counter (APERF/MPERF) delta-math unit/smoke test; runtime CSV evidence on a supported machine. | not buildable |
| `REQ-CPUEFF-02` | T, M | Energy-counter delta → Joules/avg-power unit/smoke test; sample-id/window de-duplication; runtime CSV evidence. | not buildable |
| `REQ-CPUEFF-03` | T, R | Context-field propagation test; review vs `RUNTIME_HOME.md`. | not buildable |
| `REQ-CPUEFF-04` | T, R | Analyzer-ingest tests with old archives missing the new fields; no-false-zero test. | not buildable |
| `REQ-CPUEFF-05` | R | Review confirms read paths only; no CPU-control write. | not buildable |
| `REQ-CPUEFF-06` | R | Review of the enumerated register/counter read set vs `AGENTS.md`. | not buildable |
| `REQ-CPUEFF-07` | R | Review logger code/docs: no baked-in efficiency scoring. | not buildable |
| `REQ-CPUEFF-08` | T, R | CPU-settings label-propagation test/config review. | not buildable |

### FEAT-0005 / FEAT-0007 — Reserved (parked)

`REQ-ACTCONFIRM-*` (FEAT-0005) and `REQ-RAMTEMP-*` (FEAT-0007) are **not mirrored
here while their specs are Reserved.** Their requirement rows live in the parked
bodies under `docs/features/_parked/` and rejoin this map when a spec is promoted
back to `Draft` (see `docs/features/README.md` §5). This map mirrors only the
`REQ-*` IDs of the active, enforced feature specs, which is what
`tests/test_feature_specs.py` requires.
