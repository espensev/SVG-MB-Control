# svg-mb-control - Traceability

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.5   **Updated:** 2026-06-18
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
| `FEAT-0004` Hardware-access dependency health signal | Accepted | Buildable when implementation is explicitly authorized; verification pending. Promoted Draft→Accepted 2026-06-18; decision record `docs/hwaccess-health-signal-decision-2026-06-18.md` Current (additive observability; exit codes unchanged). |
| `FEAT-0005` Write actuation confirmation | Accepted | Buildable when implementation is explicitly authorized; verification pending. Un-parked and promoted Reserved→Accepted 2026-06-18; decision `docs/actuation-confirmation-decision-2026-06-18.md` Current. Accepted scope is Phase-1 RPM-based detection/evidence; Phase-2 escalation is a separate, measurement-gated step. |
| `FEAT-0006` CPU work and energy efficiency evidence | Accepted | Promoted Draft→Accepted 2026-06-07 (all gates met). The one-shot read-only live MSR validation ran 2026-06-07 — PASS (energy-only): RAPL package energy works on Family 1Ah; the APERF/MPERF `#GP` was corrected 2026-06-09 by using the shipped AMD read-only aliases. Energy, cycle, and analyzer derivations have landed behind default-off gates. Enabled integration sessions 1-3 passed (each 5 PASS / 0 FAIL / 1 MANUAL); the unsupported fixed >=7-day span was removed 2026-06-14. Energy quarantine-exit evidence is complete; marker promotion remains a manual maintainer decision. FEAT-0004 recommended, not blocking. |
| `FEAT-0007` RAM temperature telemetry | Reserved (body parked) | Not buildable; body parked. Read path exists (SVG-MB-SIO `read_sio_temperatures` DIMM sources); promotion would require DIMM-source validity confirmation from `evidence-log` plus a sampling/schema decision. |
| `FEAT-0008` Watchdog hung-worker recovery | Done | The bounded force-terminate escalation landed in `src/control/worker_force_terminate.{h,cpp}` (the `Win32ProcessTerminator` calls `TerminateProcess`) plus the `app_main.cpp` `--restart` `stop_result == 2` branch; C++ unit + Python suspend-based integration tests pass (CTest + pytest green); the recovery **mechanism** is also verified live (M) on the deployed build via an `NtSuspendProcess` hung-worker proxy (REQ-WATCHDOG-01). Decision record current (`docs/watchdog-hung-worker-recovery-decision-2026-06-16.md`, all 7 gates met). No v1 recovery-path work remains: the separate natural-hard-freeze premise was closed on evidence as not reproducible by load on this system (n=6 aggressive cells, 0 force-terminations), and the AVX-512 escalation was rejected as the wrong instrument for FEAT-0008; post-v1 options live in FEAT-0008 §11. |
| `FEAT-0009` Controller scheduling-priority elevation | Draft (held) | Not buildable; design capture only, held at promotion gate 1 pending the FEAT-0009 §12 A/B contention experiment (whether the cadence degradation under `above`-load is scheduling-bound rather than file-lock bound). |
| `FEAT-0010` Write actuation survives a sidecar-persistence fault | Done | Implemented 2026-06-17 (decision record `docs/write-actuation-sidecar-fault-decision-2026-06-17.md` current). Fixes runtime-reproduced finding H1: a `pending_writes.json` persist fault no longer vetoes the fan write (incl. the sensor-safe command); it records `control_loop.sidecar_upsert_failed`, increments `consecutive_sidecar_persist_failures` (degrades health), and still actuates. §14 verification log filled; CTest 13/13 green. |
| `FEAT-0011` Write-failure breaker must not block rising cooling demand | Done | Implemented 2026-06-17 (decision `docs/breaker-probe-decision-2026-06-17.md` Current). A bounded half-open probe at `channel_write.cpp:307`: while the breaker is open, a rising cooling demand (setpoint above last applied duty) triggers at most one probe write per 5 s (`kBreakerProbeBackoff`); a successful probe closes the breaker, a failed one keeps it open. `safety_override` unchanged; additive `circuit_breaker_probe` event. §14 filled; CTest green. |
| `FEAT-0012` Startup tolerates a corrupt pending-writes sidecar | Done | Implemented 2026-06-17 (Direction A; decision `docs/corrupt-sidecar-quarantine-decision-2026-06-17.md` Current). A corrupt `pending_writes.json` is quarantined to `pending_writes.json.corrupt` and the startup reconcile proceeds as empty instead of aborting into a relaunch-thrash loop; emits `reconcile.sidecar_quarantined` and degrades health (`sidecar_quarantined_present`). Collapses the former duplicate sidecar read. §14 filled; CTest + pytest green. |
| `FEAT-0013` Source-aware primary-dropout safe mode | Done | Implemented 2026-06-17 (decision `docs/source-aware-cpu-dropout-decision-2026-06-17.md` Current). A CPU dropout on a `max_cpu_gpu_source_aware` channel (CPU seen, now gone, GPU present) now counts toward the existing 3-miss sensor-failure trip and enters safe mode (`safety_override` + response source `source_aware_cpu_dropout_safe_mode`), reusing the `CpuOnly` mechanism; recovers on CPU return. Only new state is the additive `cpu_input_was_available` flag. §14 filled; CTest green. |
| `FEAT-0014` Reconcile and restore honor the blocked-channel guard | Draft (held) | Not buildable; investigated code/contract gap, held at promotion gate 3 (no decision record) pending maintainer direction on guard placement and retain-vs-clear. The restore/reconcile path omits the `channel_blocked`/`writes_enabled` check `set_fan_duty` enforces, but a blocked-channel sidecar entry is unreachable under the shipped single-profile config and the fail direction is bounded/one-shot. |
| `FEAT-0015` Event JSONL retention | Implemented | Implemented 2026-06-18 (decision record `docs/event-log-retention-decision-2026-06-18.md` Current). Event JSONL rotates on the configured age/retention window, routine `control_loop.write_applied` info events are sampled, diagnostics/lifecycle events remain persisted, and full `Test-LocalCI` passed. |
| `FEAT-0016` Analyze SQLite DB retention | Implemented | Implemented 2026-06-18 (decision record `docs/analyze-db-run-purge-decision-2026-06-18.md` Current). `analyze prune --db-retain-days` purges old runs under foreign keys, verifies no orphans, reclaims space after deletes, and full `Test-LocalCI` passed. Immediate derived DB reclaim completed 2026-06-18. |
| `FEAT-0017` Faster fan reaction under load (control-response retune) | Draft (held) | Not buildable; design capture (`docs/control-latency-reduction-design-2026-06-18.md` D-REACT-1, Proposed). Config-only joint rise-rate + step-cap raise, asymmetric, lane-targeted; held pending the decision on lanes/target ceiling and a response-evaluation Pass-3 validation. Does not cross the measurement gate. |
| `FEAT-0018` Adaptive-cadence enablement under thermal transient | Draft (held) | Not buildable; design capture (D-CADENCE-1, Proposed). Engages the dormant `poll_tick_floor_ms` engine; **crosses `MEASUREMENT_GATE.md`** (adaptive floor below the shipped profile), held until the floor characterization evidence exists. |
| `FEAT-0019` Sidecar persistence off the actuation hot path | Implemented | Implemented 2026-06-18 (D-WRITEHOT-1 Current). Identity-gated `Persist()` removes the per-tick sidecar write from the actuation hot path (sync only on a baseline-identity change; same-baseline churn deferred to the end-of-tick `Flush()`); behavior-preserving for recovery. REQ-WRITEHOT-06 two-point counter reset (Upsert bool + Flush bool/tick_runner). T/R verified; CTest 14/14. |
| `FEAT-0020` Standard control-loop power logging | Implemented | Implemented 2026-06-18 (D-PWRLOG-1 Current; full Test-LocalCI green — CTest + 169 hermetic). CPU side reuses the FEAT-0006 RAPL path (env flip only, no worker code); GPU side adds a per-tick cadence-agnostic 5-field power slice with a read-timestamp, summarized by the analyzer (v11) as mean/percentile (not an energy integral). `T`/`B`/`R` verified; the `M` runtime cadence evidence (REQ-PWRLOG-04) and the live flip stay deferred to a separately authorized live-runtime window. Implementation plan: `docs/feat-0020-power-logging-implementation-plan-2026-06-18.md`. |
| `FEAT-0021` Standard control-loop GPU workload context logging | Draft | Not buildable yet; D-GPUCTX-1 (`docs/logging-next-targets-2026-06-18.md`) is Proposed, and combined GPU power/context sample cadence evidence is pending. Sequenced behind FEAT-0020 so the immediate power-logging target can stay narrow. |

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
| `REQ-HWHEALTH-01` | T, R | Status-field test; review `RUNTIME_HOME.md`. | pending |
| `REQ-HWHEALTH-02` | T | Read-path-down vs write-path-down init outcomes set distinct fields. | pending |
| `REQ-HWHEALTH-03` | T, R | Analyzer/ingest compatibility with old status files; additive-only schema review. | pending |
| `REQ-HWHEALTH-04` | T, M | Tests assert transition events; runtime event-log evidence. | pending |
| `REQ-HWHEALTH-05` | R | Review confirms no driver load/start/restart path. | pending |
| `REQ-HWHEALTH-06` | T | No successful open means unknown/unavailable, never healthy. | pending |

### FEAT-0005 - Write actuation confirmation (non-actuating-write detection)

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-ACTCONFIRM-01` | T | Simulated channel: commanded duty vs observed RPM/duty response over a window. | pending |
| `REQ-ACTCONFIRM-02` | T | `simulated_fan_writer`: commanded-high-but-flat trips; commanded-zero-and-flat does not. | pending |
| `REQ-ACTCONFIRM-03` | R | Review vs `CONTROL_PIPELINE_MATH.md` / `WRITE_ORCHESTRATION.md`: Phase-1 changes no duty/cadence/breaker/identity. | pending |
| `REQ-ACTCONFIRM-04` | T, M | Test asserts additive fields + onset/clear events; runtime event-log evidence. | pending |
| `REQ-ACTCONFIRM-05` | T, R | Test that detection evaluates during a hold window; review vs the authority-reassert path. | pending |
| `REQ-ACTCONFIRM-06` | R | Review confirms Phase-2 escalation is absent in Phase 1 and is gated by `MEASUREMENT_GATE.md`. | pending |
| `REQ-ACTCONFIRM-07` | R | Review: in-repo `FanWriter` telemetry only; no third-party/subprocess/sibling dependency. | pending |

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

### FEAT-0010 - Write actuation survives a sidecar-persistence fault

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-WRITESAFE-01` | T | C++ test: a throwing pending-store (`ThrowingPendingWritesStore`) asserts `ApplyDuty` still fires for the computed setpoint after a persist failure (`channel_write_tests.cpp`). | pass |
| `REQ-WRITESAFE-02` | T | C++ test: `safety_override` set + throwing pending-store + open breaker asserts the 100% command reaches the actuator. | pass |
| `REQ-WRITESAFE-03` | T, R | C++ test asserts the additive per-channel counter increments/resets, health degrades (`DegradedChannelCount`), the `control_loop.sidecar_upsert_failed` event is emitted, the breaker stays closed and `consecutive_write_failures` stays 0; review vs `RUNTIME_HOME.md`. | pass |
| `REQ-WRITESAFE-04` | T, R | C++ test: a stale and an absent sidecar entry preserve the captured-baseline round-trip (`channel_write_tests.cpp`); reconcile→restore integration-covered (`test_write_once.py`); review vs `WRITE_ORCHESTRATION.md`. | pass |
| `REQ-WRITESAFE-05` | R | Review vs `CONTROL_PIPELINE_MATH.md` / `MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity unchanged; status field additive. | pass |
| `REQ-WRITESAFE-06` | T, R | C++ test: a persist failure whose event serialization throws (non-UTF-8 message) still reaches `ApplyDuty` (`TestEventLogThrowDoesNotVetoActuation`); review the pre-actuation append is wrapped best-effort. | pass |

### FEAT-0011 - Write-failure breaker must not block rising cooling demand

Held at Draft; verification homes are planned, results `pending (held-Draft)` until
the maintainer authorizes promotion (gate 3).

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-COOLWRITE-01` | T | `channel_write_tests.cpp::TestRisingDemandProbesOpenBreaker`: an open-breaker channel with a rising setpoint asserts `ApplyDuty` fires (probe). | pass |
| `REQ-COOLWRITE-02` | T | `channel_write_tests.cpp::TestNonCoolingWriteSuppressedByOpenBreaker` (lower setpoint stays suppressed) + `TestSensorSafeBypassesOpenBreaker` (`safety_override` bypass unchanged). | pass |
| `REQ-COOLWRITE-03` | T, R | `TestProbeSuccessClosesBreaker` (success closes the breaker, resets `consecutive_write_failures`) + `TestProbeFailureKeepsBreakerOpen` (failure leaves it open); reuses the existing self-heal path. | pass |
| `REQ-COOLWRITE-04` | T, R | `TestProbeRateLimitedWithinBackoff`: a second rising write within the 5 s `kBreakerProbeBackoff` does not probe again; the bound is in `docs/breaker-probe-decision-2026-06-17.md`. | pass |
| `REQ-COOLWRITE-05` | R | Review vs `CONTROL_PIPELINE_MATH.md` / `MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity unchanged; only the internal `last_probe_time` is new and the `circuit_breaker_probe` event is additive. | pass |

### FEAT-0012 - Startup tolerates a corrupt pending-writes sidecar

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-SIDECARRESIL-01` | T | `channel_write_tests.cpp::TestReconcileQuarantinesCorruptSidecarAndProceeds` (reconcile returns 0, no abort) + `...CorruptSidecarIsQuarantinedAndReadProceeds` + `...NonArrayEntries...`; `test_smoke.py::test_corrupt_pending_writes_is_quarantined_and_startup_proceeds`. | pass |
| `REQ-SIDECARRESIL-02` | T | `channel_write_tests.cpp::TestCorruptSidecarIsQuarantinedAndReadProceeds` asserts the corrupt bytes are preserved at `pending_writes.json.corrupt`; `test_smoke.py` asserts the same on the live stack. | pass |
| `REQ-SIDECARRESIL-03` | T, R | `TestReconcileQuarantinesCorruptSidecarAndProceeds` + `test_smoke.py` assert `reconcile.sidecar_quarantined`; the `sidecar_quarantined_present` health flag degrades health + is in the `--health` JSON (`runtime_health.cpp`); review vs `docs/RUNTIME_HOME.md`. | pass |
| `REQ-SIDECARRESIL-04` | T, R | `channel_write_tests.cpp::TestValidSidecarIsNotQuarantined` (valid sidecar unchanged); happy-path reconcile/restore unchanged (`test_write_once.py` stays green); review vs `docs/WRITE_ORCHESTRATION.md`. | pass |
| `REQ-SIDECARRESIL-05` | R | Review vs `docs/MEASUREMENT_GATE.md`, `docs/CONTROL_PIPELINE_MATH.md`, and FEAT-0008: change confined to the startup read/reconcile path; write path/durability/relaunch/cadence/channels/identity unchanged; event + artifact + health field additive. | pass |

### FEAT-0013 - Source-aware channels enter safe mode on primary-source dropout

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-SRCSAFE-01` | T | `channel_write_tests.cpp::TestSourceAwareCpuDropoutCountsTowardTrip`: CPU-available then dropped while GPU remains asserts `consecutive_sensor_failures` increments rather than resetting. | pass |
| `REQ-SRCSAFE-02` | T | `channel_write_tests.cpp::TestSourceAwareCpuDropoutTripsSafeMode`: three CPU-dropout ticks set `safety_override` + response source `source_aware_cpu_dropout_safe_mode`. | pass |
| `REQ-SRCSAFE-03` | T, R | `...TripsSafeMode` asserts `ChannelSensorEvent::FailureDetected`; `...CpuRecoveryClearsDropout` asserts `Recovered` on CPU return; reuses the existing `RUNTIME_HOME.md` event list. | pass |
| `REQ-SRCSAFE-04` | T | `channel_write_tests.cpp::TestSourceAwareNeverPresentCpuDoesNotTrip`: a GPU-led channel that never had CPU does not trip; a channel that had CPU and lost it does. | pass |
| `REQ-SRCSAFE-05` | R | `TestSourceAwareBothPresentNoTrip` + review: both-inputs-present duty/cadence/channels/identity and total-loss behavior unchanged; only the additive `cpu_input_was_available` flag is new. | pass |

### FEAT-0014 - Reconcile and restore honor the blocked-channel guard

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-RESTOREGUARD-01` | T, R | `ReconcilePendingWrites` skips `RestoreSavedState` for a policy-blocked channel; test with a call-recording/simulated writer + `blocked_channels` policy; review vs `svg_mb_sio.cpp:218-223` guard parity. | pending (held-Draft) |
| `REQ-RESTOREGUARD-02` | T, R | Blocked-channel skip emits `reconcile.restore_skipped_blocked` and does not fail the reconcile exit code; review vs `docs/WRITE_ORCHESTRATION.md` Reconciliation. | pending (held-Draft) |
| `REQ-RESTOREGUARD-03` | T, R | `RunWriteOnce`/restore path does not write a baseline to a blocked channel; review that the existing `write_orchestrator.cpp:151-165` exit-5 refusal is preserved. | pending (held-Draft) |
| `REQ-RESTOREGUARD-04` | T | Unblocked-channel restore is byte-for-byte unchanged (FEAT-0010 crash-recovery replay regression guard). | pending (held-Draft) |
| `REQ-RESTOREGUARD-05` | R | Review vs `CONTROL_PIPELINE_MATH.md` / `MEASUREMENT_GATE.md`: computed duty/cadence/live-channel set/identity unchanged; event additive, no schema break. | pending (held-Draft) |

### FEAT-0015 - Event JSONL retention

Implemented 2026-06-18. Results mirror the owning spec's §14 verification log.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-EVENTRET-01` | T, R | `runtime_event_log_tests.cpp::TestRotationAndArchivePrune` verifies age rotation/archive pruning; `TestWriteAppliedReductionPreservesDiagnostics` verifies routine `write_applied` sampling; review vs decision record. | pass |
| `REQ-EVENTRET-02` | T, R | `TestConcurrentAppendRotationKeepsWholeLines` parses every active/archive line as JSON after concurrent appends across a rotation boundary; review confirms single-call append and in-process rotation/append serialization. | pass |
| `REQ-EVENTRET-03` | T | `TestWriteAppliedReductionPreservesDiagnostics` verifies routine `write_applied` ticks are reduced while `control_loop.write_failed` and `control_loop.shutdown` remain persisted. | pass |
| `REQ-EVENTRET-04` | T, R | Runtime event-log tests plus review: event payload schema stays `svg_mb_control.event.v1`, unconfigured append paths keep historical behavior, and `CachedEventCount` still reads the active file. | pass |
| `REQ-EVENTRET-05` | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` / `docs/MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity and CSV retention unchanged; event logging remains best-effort and docs updated. | pass |

### FEAT-0016 - Analyze SQLite DB retention

Implemented 2026-06-18. The 2026-06-18 operational reclaim deleted the derived
analyzer DB to free 7.80 GiB; the structural DB purge/reclaim code now lands in
`analyze prune`.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-DBRETAIN-01` | T, R | `test_analyze_ingest.py::test_db_prune_apply_cascades_and_reclaims` ingests old/recent runs, applies `--db-retain-days 1`, and asserts only the recent run remains; review vs decision record. | pass |
| `REQ-DBRETAIN-02` | T | `test_db_prune_apply_cascades_and_reclaims` asserts dependent `tick_samples`, `tick_fan_samples`, `tick_channel_samples`, and `events` rows for the deleted run are removed; implementation treats orphan rows as an error. | pass |
| `REQ-DBRETAIN-03` | T | `test_db_prune_apply_cascades_and_reclaims` asserts page/file-size shrink after deleting old runs; `test_db_prune_zero_retain_days_is_explicit_disable` verifies the disable path. | pass |
| `REQ-DBRETAIN-04` | T, R | `test_db_prune_dry_run_keeps_old_run` verifies dry-run reports without deleting; apply test re-runs ingest after purge and confirms retained runs still de-duplicate. | pass |
| `REQ-DBRETAIN-05` | R | Review vs `docs/RUNTIME_HOME.md` / `docs/MEASUREMENT_GATE.md`: analyze schema/version, per-tick fidelity, and existing CSV-bundle prune unchanged; README/runtime docs updated. | pass |

### FEAT-0017 - Faster fan reaction under load (control-response retune)

Held at Draft; verification homes are planned, results `not buildable` until the
decision record settles the lanes/target ceiling (gate 3) and a response-evaluation
Pass-3 validation exists.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-REACT-01` | T, R | Config-contract test that each retuned lane raised both `rise_rate_pct_per_min` and `max_setpoint_step_pct` (effective rise ceiling rose); review the ceiling identity vs `docs/control-latency-reduction-design-2026-06-18.md` §2.1. | not buildable |
| `REQ-REACT-02` | T, R | Config-contract test that no retuned lane raised `fall_rate_pct_per_min` / `demand_smoothing_fall_alpha` / `decay_latch_pct_per_min` above the shipped value (rise-asymmetry). | not buildable |
| `REQ-REACT-03` | R | Review the config diff vs `MEASUREMENT_GATE.md` / `CONTROL_PIPELINE_MATH.md`: curves, blend, channels, cadence, cooldown, deadband, overlays unchanged. | not buildable |
| `REQ-REACT-04` | M | Live combined-load (Pass 3) capture via `analyze ingest` + `analyze report`: before/after reaction improved; CPU Tctl / GPU memory percentiles within band; no post-startup authority reasserts. | not buildable |
| `REQ-REACT-05` | T | `tests/test_config_contracts.py::test_release_intake_low_end_curves_follow_machine_policy` stays green (channels `2`/`3` keep `>= 4%` spacing). | not buildable |

### FEAT-0018 - Adaptive-cadence enablement under thermal transient

Held at Draft; **crosses the measurement gate**, results `not buildable` until the
floor characterization evidence exists (gate 6) and the decision record settles the
floor/thresholds (gate 3).

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-CADENCE-01` | T, M | Config-load test that the shipped config sets `poll_tick_floor_ms` in `[25, poll_tick_ms)`; existing `ComputeCadence` unit tests cover full-slew→floor; (M) a transient capture reaches the floor. | not buildable |
| `REQ-CADENCE-02` | T | Config-load / `ComputeCadence` tests: `start < full`; relax returns to `poll_tick_ms` after slew subsides; no tighten below `cadence_slew_start_c_per_s`. | not buildable |
| `REQ-CADENCE-03` | M | Characterization capture: steady-state achieved interval, `loop_slip_ms`, `loop_overrun`, process CPU% within `MEASUREMENT_GATE.md` exit criteria. | not buildable |
| `REQ-CADENCE-04` | R, M | Review the config diff (`write_cooldown_ms`/channels unchanged); runtime evidence that write frequency stays bounded by deadband + cooldown. | not buildable |
| `REQ-CADENCE-05` | M, R | The `MEASUREMENT_GATE.md` characterization summary exists (AMD/SIO/GPU cadence + fan-write response at the floor); review the chosen floor/thresholds are justified by it. | not buildable |

### FEAT-0019 - Sidecar persistence off the actuation hot path

Implemented 2026-06-18 (T/R verified; isolated Test-LocalCI green — CTest 14/14,
169 hermetic, `test_feature_specs` 5/5). D-WRITEHOT-1 promoted to Current (gate 3).
The change strictly reduces synchronous writes and is behavior-preserving for
recovery, so no `M` evidence is required by §10. Results mirror the owning spec's
§14 verification log.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-WRITEHOT-01` | T | C++ test (sidecar file-existence over a temp runtime home): an `Upsert` changing only `target_pct` on a same-baseline entry performs no synchronous file write; a first entry and a baseline change each persist one synchronously. | pass |
| `REQ-WRITEHOT-02` | T, R | C++ test: a sidecar from several same-baseline `Upsert`s reconciles to restore each channel's baseline; review `ReconcilePendingWrites` consumes only `(channel, baseline_duty_raw, baseline_mode_raw)`. | pass |
| `REQ-WRITEHOT-03` | T | C++ test: the first `Upsert` for a channel persists before the simulated `ApplyDuty` (activation record on disk before actuation). | pass |
| `REQ-WRITEHOT-04` | T | C++ test: a skipped same-baseline update is written by `Flush()`; `QueueRemove` + `Flush` removal path unchanged; a baseline re-capture triggers a fresh synchronous persist. | pass |
| `REQ-WRITEHOT-05` | R | Review `docs/RUNTIME_HOME.md` (clarified `target_pct`/`started_iso` ≤1-tick-stale/advisory semantics; schema unchanged) and that no consumer reads `target_pct` as authoritative per-tick current. | pass |
| `REQ-WRITEHOT-06` | T, R | C++ test: after a forced identity-change persist failure, a following same-baseline `Upsert` does not reset `consecutive_sidecar_persist_failures`; review the changed `channel_write.cpp` reset site vs FEAT-0010. | pass |

### FEAT-0020 - Standard control-loop power logging

Implemented 2026-06-18 (T/B/R/M verified; full Test-LocalCI green — CTest + 169
hermetic). Results mirror the owning spec's §14 verification log. The live-runtime
flip was executed under explicit authorization on 2026-06-18 (deploy of commit
`1ea44c7` via `Build-Release.ps1` + `Set-EnergyLoggingProfile.ps1 -Enable`),
capturing the `M` evidence for `REQ-PWRLOG-01/-02/-04/-06` (gate 6 closed). Live
results: `docs/feat-0020-live-flip-validation-results-2026-06-18.md`.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-PWRLOG-01` | T, M, R | Config/script test for the CPU RAPL profile (`test_energy_logging_profile.py`); live M 2026-06-18: `cpu_pkg_energy_acquisition=quarantine` with `cpu_power_sample_id` populating (run 318, analyzer `package_power` avg 86.74 W); review vs FEAT-0006 marker semantics. | pass |
| `REQ-PWRLOG-02` | T, M, R | CSV header/row tests (`csv_rows_tests`, `test_control_loop.py`); analyzer ingest (`test_analyze_ingest.py`); live M 2026-06-18: `gpu_power_mw` nonempty (`acquisition`/`source=nvml`, 1658/1659; one leading `unavailable`, no false zero); no-false-zero review. | pass |
| `REQ-PWRLOG-03` | T, R | `test_control_loop.py` asserts response source stays `primary_curve`; review power is not in `TempInputs`/`EvaluateChannel` and `power_anticipation.h` stays unwired; `CONTROL_PIPELINE_MATH.md` unchanged. | pass |
| `REQ-PWRLOG-04` | M, R | Review: one `nvmlDeviceGetPowerUsage` piggybacked on the per-tick thermal sample (`poll_nvml_board_power`); live M 2026-06-18: post-flip `loop_work_duration_ms` steady-state mean 3.45 ms / max 34.9 ms and, under GPU load ≥350 W (648 ticks), mean 3.15 ms / max 14.6 ms, 0 overruns vs the 250 ms period — the read does not move the baseline; residual idle-only spikes are pre-existing (old build max 3274 ms). Results: `docs/feat-0020-live-flip-validation-results-2026-06-18.md`. | pass |
| `REQ-PWRLOG-05` | T, R | Analyzer tests for the GPU distribution (mean, not energy integral), old-archive degrade, and v10→v11 migration; review CPU watts derivation unchanged, not double-logged. | pass |
| `REQ-PWRLOG-06` | T, M, R | `test_energy_logging_profile.py` dry-run flip/revert (`-Enable`/`-Disable`); live M 2026-06-18: `-Enable` disabled the `SVG-MB Energy Safety Revert` task (→ `Disabled`), set User `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`, and restarted the worker tree; `-Disable` reversibility documented; README/operator docs reviewed. | pass |

### FEAT-0021 - Standard control-loop GPU workload context logging

Held at Draft; results `not buildable` until D-GPUCTX-1 is promoted to Current
and combined GPU power/context sample cadence evidence exists. Sequenced behind
FEAT-0020.

| Requirement | Verify | Verification home | Result |
|---|---|---|---|
| `REQ-GPUCTX-01` | T, R | CSV header/row tests for the context fields; review field names against `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md`. | not buildable |
| `REQ-GPUCTX-02` | T, R | Unit/integration seam test or review proving the context is sourced from the in-repo GPU reader/evidence path and does not require `evidence-log` foreground mode. | not buildable |
| `REQ-GPUCTX-03` | T, R | Control-loop tests/review showing context fields are not read by setpoint, boost, response-source, write, breaker, or safety paths. | not buildable |
| `REQ-GPUCTX-04` | M, R | Runtime evidence from the standard 250 ms profile: achieved interval, slip/overrun, process CPU%, and health stay inside the current measurement envelope. | not buildable |
| `REQ-GPUCTX-05` | T, R | Analyzer ingest/report tests for archives with and without the new fields; review report output treats context as optional. | not buildable |
| `REQ-GPUCTX-06` | T, R | CSV header tests and review confirm wide diagnostic fields remain out of the v1 standard-log slice. | not buildable |

### FEAT-0007 — Reserved (parked)

`REQ-RAMTEMP-*` (FEAT-0007) is **not mirrored here while its spec is Reserved.**
Its requirement rows live in the parked body under `docs/features/_parked/` and
rejoin this map when the spec is promoted back to `Draft` (see
`docs/features/README.md` §5). FEAT-0005 was un-parked and promoted to
`Accepted`; its `REQ-ACTCONFIRM-*` rows are mirrored above. This map mirrors only
the `REQ-*` IDs of the active, enforced feature specs, which is what
`tests/test_feature_specs.py` requires.
