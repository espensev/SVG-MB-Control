# svg-mb-control - Traceability

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.4   **Updated:** 2026-06-16
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
- **partial** = implemented and verified for a defined sub-scope; the remainder
  is deferred or pending.
- **not buildable** = the spec is not implementation-authorized yet because it
  has open promotion gates, is a held Draft/design capture, is parked, or has an
  explicit pre-implementation validation gate.

## 2. Feature status

| Feature | Status | Buildability |
|---|---|---|
| `FEAT-0001` Hot-swap runtime write policy | Accepted | Buildable when implementation is explicitly authorized; verification pending. |
| `FEAT-0002` CPU settings evidence logger | Implemented (source/test load layer; label deferred) | Source/test requirements pass except `REQ-CPUSETTINGS-06`, which is deferred; the 2026-06-09 rebuild confirmed the `system_cpu_*` columns in the live CSV header (git_hash `dd2c02214128`, session `2026-06-09T02:32:40`), closing the packaging-evidence gap. |
| `FEAT-0003` Selectable control-law profile with hot-swap | Draft | Not buildable; design capture only. |
| `FEAT-0004` Hardware-access dependency health signal | Draft | Not buildable; decision record gate open. |
| `FEAT-0005` Write actuation confirmation | Reserved (body parked) | Not buildable; body parked in `docs/features/_parked/`, sequenced behind FEAT-0004. |
| `FEAT-0006` CPU work and energy efficiency evidence | Accepted | Promoted Draft→Accepted 2026-06-07 (all gates met). The one-shot read-only live MSR validation ran 2026-06-07 — PASS (energy-only): RAPL package energy works on Family 1Ah; the APERF/MPERF `#GP` was corrected 2026-06-09 by using the shipped AMD read-only aliases. Energy, cycle, and analyzer derivations have landed behind default-off gates. Enabled integration sessions 1-3 passed (each 5 PASS / 0 FAIL / 1 MANUAL); the unsupported fixed >=7-day span was removed 2026-06-14. Energy quarantine-exit evidence is complete; marker promotion remains a manual maintainer decision. FEAT-0004 recommended, not blocking. |
| `FEAT-0007` RAM temperature telemetry | Reserved (body parked) | Not buildable; body parked. Read path exists (SVG-MB-SIO `read_sio_temperatures` DIMM sources); promotion would require DIMM-source validity confirmation from `evidence-log` plus a sampling/schema decision. |
| `FEAT-0008` Watchdog hung-worker recovery | Implemented | The bounded force-terminate escalation landed in `src/control/worker_force_terminate.{h,cpp}` (the `Win32ProcessTerminator` calls `TerminateProcess`) plus the `app_main.cpp` `--restart` `stop_result == 2` branch; C++ unit + Python suspend-based integration tests pass (CTest + pytest green); the recovery **mechanism** is also verified live (M) on the deployed build via an `NtSuspendProcess` hung-worker proxy (REQ-WATCHDOG-01). Decision record current (`docs/watchdog-hung-worker-recovery-decision-2026-06-16.md`, all 7 gates met). No v1 recovery-path work remains: the separate natural-hard-freeze premise was closed on evidence as not reproducible by load on this system (n=6 aggressive cells, 0 force-terminations), and the AVX-512 escalation was rejected as the wrong instrument for FEAT-0008; post-v1 options live in FEAT-0008 §11. |
| `FEAT-0009` Controller scheduling-priority elevation | Draft (held) | Not buildable; design capture only, held at promotion gate 1 pending the FEAT-0009 §12 A/B contention experiment (whether the cadence degradation under `above`-load is scheduling-bound rather than file-lock bound). |

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
| `REQ-CPUSETTINGS-02` | T | System CPU delta calculation unit/smoke test; live package header confirmed by the 2026-06-09 rebuild. | pass |
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
| `REQ-CPUEFF-01` | T, M | Work-counter (APERF/MPERF) delta-math unit/smoke test; runtime CSV evidence on a supported machine. | partial — cycle logger landed default-off (`cpu_cycles.h` math + `cpu_cycles_tests`; `amd_reader` per-core APERF/MPERF reads logging `cpu_aperf_delta` / `cpu_mperf_delta`); analyzer effective-frequency derivation landed 2026-06-10 (schema v10). Enabled live CSV sessions 1-3 captured `cpu_cycles_acquisition=quarantine`; criterion 4 remains MANUAL in the evidence notes, so cycle promotion and cycles-per-Joule join remain pending; `INST_RETIRED` stays out of read-only scope. |
| `REQ-CPUEFF-02` | T, M | Energy-counter delta → Joules/avg-power unit/smoke test; sample-id/window de-duplication; runtime CSV evidence. | pass (marker promotion pending) — energy logger landed 2026-06-07; analyzer time-weighted avg-power derivation landed 2026-06-09 (schema v9, sample-id de-duplication, no-false-zero; `ComputePackagePower` unit tests + `test_analyze_ingest` end-to-end). Enabled live CSV sessions 1-3 captured `cpu_pkg_energy_acquisition=quarantine`; each scored 5 PASS / 0 FAIL / 1 MANUAL, with the MANUAL item limited to cycle effective-frequency validity. Energy quarantine-exit evidence is complete across 3 independent sessions; flipping to `validated` remains a manual maintainer decision. |
| `REQ-CPUEFF-03` | T, R | Context/provenance propagation test, explicit deferred-signal unavailable handling, and review vs `RUNTIME_HOME.md`. | pass (v1 scope) — temperature stays aligned with the window; energy and cycle acquisition markers remain independent; effective-frequency inputs are emitted only when the default-off cycle path is enabled and are derived by analyzer schema v10 rather than guessed. |
| `REQ-CPUEFF-04` | T, R | Analyzer-ingest tests with old archives missing the new fields; no-false-zero test. | pass — additive nullable columns; `test_analyze_ingest` ingests subset/old archives; no-false-zero via the implausibility guard |
| `REQ-CPUEFF-05` | R | Review confirms read paths only; no CPU-control write. | pass — the energy and cycle helpers issue `ioctl_read_msr` only; no write in the FEAT-0006 paths |
| `REQ-CPUEFF-06` | R | Review of the enumerated register/counter read set vs `AGENTS.md`. | pass — enumerated read sets `{0xC0010299, 0xC001029B}` via `rapl::IsAllowlistedEnergyMsr` and `{0xC00000E7, 0xC00000E8}` via `cycles::IsAllowlistedCycleMsr`, with allow-list guard tests |
| `REQ-CPUEFF-07` | R | Review logger code/docs: no baked-in efficiency scoring. | pass — logger records raw delta+window only; no efficiency scoring (review) |
| `REQ-CPUEFF-08` | T, R | CPU-settings label-propagation test/config review. | deferred — workload/CPU-setting label is the shared open question with FEAT-0002 §8; not in v1 |

### FEAT-0008 - Watchdog hung-worker recovery

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-WATCHDOG-01` | T, M | (T) `test_hung_worker_is_force_terminated_and_relaunched` (suspended worker force-terminated; relaunched PID differs); (M) live deploy 2026-06-16 commit `e5bafdb`: suspended live worker pid 44984 force-terminated by the production watchdog `--restart` (`supervisor.worker_force_terminated`, stop_result=2), fresh worker pid 36348, loop resumed ticking. M verifies the recovery **mechanism** under an `NtSuspendProcess` proxy (the deterministic hung-worker trigger); whether a natural 15 s+ hard freeze occurs by load is resolved on evidence — by mechanism the scheduling axis cannot produce it (timer-bound stop poll + balance-set boost + watchdog asymmetry), corroborated by n=6 aggressive cells with 0 force-terminations (`cpu-0609-freeze-classification-2026-06-16.md`; `cpu-loop-survival-live-sweep-findings-2026-06-16.md` Appendix C). Not a recovery-path gap. | pass (T, M) |
| `REQ-WATCHDOG-02` | T, R | Same integration test asserts the `supervisor.worker_force_terminated` event records the killed PID; review vs the additive `supervisor.*force_terminate*` event types in `RUNTIME_HOME.md`. | pass |
| `REQ-WATCHDOG-03` | T, R | Same integration test seeds an orphaned `pending_writes.json` entry the force-killed relaunch reconciles to `[]`; review that the escalation leaves the `app_main.cpp` startup `ReconcilePendingWrites` path unchanged. | pass |
| `REQ-WATCHDOG-04` | T, R | `test_graceful_worker_is_not_force_terminated` (no escalation on a graceful stop) + `tests/cpp/worker_force_terminate_tests.cpp` (image-guard / PID-corroboration refusal, single-shot bound); trigger gated on `stop_result == 2`. | pass |

### FEAT-0009 - Controller scheduling-priority elevation

Held at Draft; verification homes are planned, results are `pending (held-Draft)`
until the FEAT-0009 §12 A/B contention experiment authorizes promotion.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-PRIORITY-01` | T, R | `ProcessPriority` seam unit test (level→class/thread mapping, raise-only, `REALTIME` unreachable) + review of the `app_main.cpp` startup apply site. | pending (held-Draft) |
| `REQ-PRIORITY-02` | T, R | `process_priority` enum/default config-parse test; review that a non-aggressive value disables the elevation (relaunch-applied, no rebuild). | pending (held-Draft) |
| `REQ-PRIORITY-03` | M, R | (R) review every inter-tick/retry wait on the elevated thread is a kernel wait (`wait_until` in `control_scheduler.cpp` + backoff `sleep_for` in `json_io.cpp`); (M) hard promotion blocker — worker CPU% near zero between ticks while elevated. | pending (held-Draft) |
| `REQ-PRIORITY-04` | T, M, R | Recovery-against-elevated-worker test (FEAT-0008 force-terminate at priority 15); (M) live force-terminate of an elevated suspended worker; (R) review that the `--restart` killer process and supervisor are elevated (a raised task `<Priority>` does not propagate to the killer). | pending (held-Draft) |
| `REQ-PRIORITY-05` | M, R | (M) §12 experiment shows no system-wide responsiveness regression / no `Global\Access_PCI` stall increase; review that no spin is held across the mutex/file-I/O wait. | pending (held-Draft) |
| `REQ-PRIORITY-06` | M | (M) §12 A/B experiment result; promotion blocked until it shows a scheduling-attributable degradation reduction. | pending (held-Draft) |

### FEAT-0005 / FEAT-0007 — Reserved (parked)

`REQ-ACTCONFIRM-*` (FEAT-0005) and `REQ-RAMTEMP-*` (FEAT-0007) are **not mirrored
here while their specs are Reserved.** Their requirement rows live in the parked
bodies under `docs/features/_parked/` and rejoin this map when a spec is promoted
back to `Draft` (see `docs/features/README.md` §5). This map mirrors only the
`REQ-*` IDs of the active, enforced feature specs, which is what
`tests/test_feature_specs.py` requires.
