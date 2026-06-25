# Review-Derived Next Steps

## Status

Reviewed and rewritten 2026-06-09; FEAT-0008 status refreshed 2026-06-16; the
write-path safety review (FEAT-0010–0014) was **closed 2026-06-17** (see below).
FEAT-0020 shipped and the runtime disk-growth retention specs (FEAT-0015/0016)
were promoted 2026-06-18. FEAT-0021 (GPU workload context) and FEAT-0022 (runtime
logging failure visibility) shipped 2026-06-20; FEAT-0023 (machine profiles +
restart switch) shipped 2026-06-21 (T/R; live M deferred) and FEAT-0003
(control-law / PID seam) is Done as of 2026-06-22. Its all-channel PID replay is
rejected by `docs/pid-shadow-characterization-2026-06-21.md`; its channel-0 live
gate evidence is `docs/pid-live-channel0-evidence-2026-06-22.md`. The
authoritative per-feature status is `docs/features/README.md` §5 and its
"## Current priority" block; this file is topical background.
These are review notes and a backlog, not work
authorization: per `AGENTS.md` (Feature Intake Gate), product-code work for a new
capability, schema/status/log field, CLI surface, or shipped-config behavior must
go through an implementation-authorized `docs\features\FEAT-*` spec first. Each
item below names the governing doc or spec.

This supersedes the earlier agent-generated list. That list was checked item by
item against the current repo; the correction is recorded under "Prior list" so
the reasoning is auditable.

## Grounded backlog

### Write-path safety review (FEAT-0010–0014) — CLOSED 2026-06-17

The 2026-06-17 external review of the control/write safety path was worked to its
acceptance bar and **closed**. All blocking and recommended-decidable findings
are implemented, merged, and CI-verified on `main`; each is `Implemented` with a
current decision record and `REQ-*` rows in `docs\TRACEABILITY.md` (governance
`test_feature_specs` 5/5):

- **FEAT-0010** — a sidecar-persist fault no longer vetoes the fan write (finding
  H1); v0.3 added `REQ-WRITESAFE-06` (failure-path event logging is best-effort /
  non-vetoing).
- **FEAT-0011** — bounded half-open breaker probe (one write per 5 s on rising
  cooling demand) so a recovered actuator self-heals (finding 5).
- **FEAT-0012** — a corrupt `pending_writes.json` is quarantined to
  `pending_writes.json.corrupt` and startup proceeds instead of relaunch-thrashing
  (finding H2).
- **FEAT-0013** — a CPU-input dropout on a `max_cpu_gpu_source_aware` channel now
  trips the existing safe mode instead of being masked by the GPU fallback (finding
  #1); all six live channels were affected.

Remaining, **all non-blocking** (backlog, not authorized work). Several were
closed out 2026-06-18 in Phase 0/1 of
`docs/archive/implemented-plans/campaign-complete-sensible-items-2026-06-18.md`;
the five Implemented
write-path/watchdog specs (`FEAT-0008/0010/0011/0012/0013`) were promoted
`Implemented → Done` on the `T`/`R` acceptance bar (`test_feature_specs` 5/5):

- **FEAT-0014** (`REQ-RESTOREGUARD-*`, Draft/held) — reconcile/restore does not
  consult the blocked-channel runtime policy. A blocked-channel sidecar entry is
  unreachable under the shipped single-profile config and the fail direction is
  bounded/one-shot; promote only if a multi-profile config makes it reachable. The
  concrete revisit trigger is now explicit in the spec §13 (a config with
  `blocked_channels` other than `[6]`, or a second restore caller).
- **Live (M) validation on hardware — DISPOSITIONED 2026-06-18** in
  `docs/write-path-live-validation-protocol-2026-06-18.md`: acceptance is already
  met by `T`/`R` (no §10 row requires `M`); live-M is supplementary. FEAT-0010 and
  FEAT-0012 are live-feasible (the latter via an operator-gated restart); FEAT-0011
  and FEAT-0013 have no safe production-path trigger and are proxy-only or
  `T`-only-closed. Any optional production-path live run still needs an
  operator-present cool idle window.
- **Linux-only CI nicety — DONE 2026-06-18**: `tests/test_eval_dashboard.py`
  `test_dashboard_server_help` now skips when neither `powershell` nor `pwsh` is on
  PATH (`@unittest.skipUnless`).
- **Known scoped residuals** (recorded, not defects): legacy `MaxCpuGpu` still has
  the CPU-dropout masking gap FEAT-0013 fixed for source-aware (unused live);
  FEAT-0013 safe mode is the existing rate-limited ramp to 100%, not an instant
  jump; FEAT-0012 health stays degraded until `pending_writes.json.corrupt` is
  removed (deliberate — so the lost-records signal is acknowledged).

### Control latency reduction (FEAT-0017 / FEAT-0018 / FEAT-0019) — intake 2026-06-18

A latency audit of the control path (design record
`docs\control-latency-reduction-design-2026-06-18.md`) found the
"fan reaction under load" budget is governed by three mechanisms, two of which are
dormant or conservatively tuned. The findings now split into one implemented
hot-path reduction and two held Draft specs:

- **FEAT-0019** (`REQ-WRITEHOT-*`) — **Implemented 2026-06-18**. Identity-gated
  sidecar persistence now keeps the activation/baseline persist synchronous but
  defers same-baseline setpoint churn to the end-of-tick `Flush()`, so no fsync'd
  atomic replace runs before `ApplyDuty` during a ramp. The FEAT-0010
  persist-failure counter reset was corrected with the two-point `Upsert`/`Flush`
  mechanism recorded in D-WRITEHOT-1. This item is closed as T/R-verified; live M
  evidence is not required by the spec.
- **FEAT-0017** (`REQ-REACT-*`) — config-only response retune: raise
  `rise_rate_pct_per_min` **and** `max_setpoint_step_pct` jointly (the binding
  spike constraint — not the EMA alpha), asymmetric (fast rise, slow fall),
  lane-targeted to the radiator lanes. Does **not** cross the measurement gate;
  governed by `response-evaluation-tuning-plan.md`. Held pending the lanes/target
  decision and a Pass-3 before/after validation.
- **FEAT-0018** (`REQ-CADENCE-*`) — engage the dormant adaptive-cadence floor
  (`poll_tick_floor_ms`, `cadence_score.cpp` is shipped-off via `F >= P`).
  **Crosses `MEASUREMENT_GATE.md`** (adaptive floor below the shipped profile);
  held until the characterization evidence below exists. Same gate as the
  "lower fixed cadence" option, so do that evidence pass once and it unblocks both.

Remaining order: FEAT-0017 (tuning, needs a load pass) → FEAT-0018 (needs the
cadence characterization). FEAT-0019 stays here only as closed context for the
latency audit. The before/after instrument for the remaining directions already
exists in the control-loop CSV
(`last_raw_demand_pct` / `last_smoothed_demand_pct` / setpoint, `loop_slip_ms`,
`cadence_transient`).

### Standard control-loop power/context logging (FEAT-0020 / FEAT-0021)

- **FEAT-0020** (`REQ-PWRLOG-*`) — **Implemented v0.4, live flip executed and
  validated** (`T`/`B`/`R`/`M`, gate 6 closed). CPU package power (enabled FEAT-0006
  RAPL) and a 5-field GPU board-power slice now log on the live standard control
  loop; analyzer introduced both at schema v11; the current schema is v13 — v12
  carries FEAT-0021 context and v13 the FEAT-0006 all-core cycle columns. The
  per-tick NVML read was shown not
  to move the 250 ms baseline (clean under GPU load; idle-only spikes are pre-existing
  and environmental). Live state: the `SVG-MB Energy Safety Revert` task is **disabled**
  and `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` (the D-PWRLOG-1 steady state);
  reverse with `scripts/Set-EnergyLoggingProfile.ps1 -Disable`. Evidence:
  `docs/feat-0020-live-flip-validation-results-2026-06-18.md`. The follow-up
  `docs/power-temp-comparison-snapshot-2026-06-18.md` preserves the standard-loop
  CPU package watts and GPU board watts beside temperatures for future
  comparisons. `scripts/analyze_cpu_temp_power.py` now turns those windows into
  package-power-banded CPU temperature summaries; the 2026-06-20 trend/method
  summary is folded into `docs/cpu-temp-comparison-harness.md`. PR #20.
- **FEAT-0021** (`REQ-GPUCTX-*`, Implemented 2026-06-20; T/R verified; **live M
  PASS-with-finding 2026-06-25**) — GPU workload context logs beside GPU power as a
  cached 1000 ms context slice: utilization, clocks, pstate, VRAM used/total, and
  explicit sample identity/time/age/acquisition. Analyzer schema v12 ingests and
  reports the optional context block while older archives report it unavailable.
  The REQ-GPUCTX-04 live cadence M ran 2026-06-25
  (`docs/feat-0021-live-cadence-evidence-2026-06-25.md`): the section-10-named
  envelope holds on the deployed loop (achieved-interval p99 251.97 ms, slip p99
  1.97 ms, overrun frac 7e-05, `process_cpu_pct` p99 0.156 %), and the wide NVML
  context read costs ~41 ms once per ~1000 ms inside the 250 ms budget.
  - **Follow-up (non-blocking, from the M finding):** the context read runs in-line
    on the control thread (~41 ms / ~17 % of the budget on the refresh tick), and
    multi-second stalls concentrate on the `age==0` read tick beyond chance (LIVE
    P<0.0001). Two optional improvements, neither required for the closed M: (a)
    move the GPU context read off-thread (the FEAT-0006 all-core-sweeper precedent)
    to remove the refresh-tick residual; (b) capture a longer clean LIVE window to
    bound the multi-second tail. (a) crosses runtime code, so it goes through the
    Feature Intake Gate (`AGENTS.md`) if pursued.

CPU temperature / power-evaluation follow-ups (todo; analysis-only unless a
feature spec authorizes new runtime fields):

- Keep radiator membership sourced from
  `config/machines/snd-desk.cooling.policy.json`: channels `1` and `5` are
  radiator exhaust, channel `4` is front radiator intake, and channel `6` is
  AIO/pump scope excluded from control/pressure math.
- Do not require coolant/water temperature for comparisons on this machine; no
  such sensor is currently available in the repo evidence path.
- Use radiator `fan1_rpm` / `fan4_rpm` / `fan5_rpm` plus setpoint as fan-side
  context for CPU package-power comparisons.
- Add APERF/MPERF effective-frequency context to controlled runs when explicitly
  enabled, then specify the cycles-per-joule join before treating it as an
  efficiency score.
- Add a CPU-hotspot temperature context source if a first-party one becomes
  available. The per-CCD Tdie half shipped 2026-06-21 (`b418aff`): the
  `amd_sensor_summary` per-CCD Tdie reads now flow through `scripts/control_csv.py`
  (`parse_ccd_temps`) into `scripts/analyze_cpu_temp_power.py` per-window
  `ccd1`/`ccd2`/`ccd_delta` context; no separate first-party CPU-hotspot sensor
  beyond those per-CCD reads exists yet.
- Add workload label, CPU-setting label, and external score capture for
  controlled CPU runs.
- Track PPT/TDC/EDC, throttle reason, and Vcore/VID only after a reviewed
  first-party source decision exists.
- Keep GPU power/context, case intake/exhaust temperature evidence when
  available, and subjective/acoustic notes attached to tuning runs.

### Runtime logging failure visibility (FEAT-0022) — implemented 2026-06-20

- **FEAT-0022** (`REQ-LOGHEALTH-*`, Implemented) —
  current evidence-integrity target promoted from the 2026-06-20 startup
  investigation and the older EH-2/EH-3/EH-4/EH-5 discovery finding. Slice A is
  shipped: `RuntimeCsvLogger` records sink/detail for CSV/archive/mirror/manifest
  failures; control-loop, read-loop, and evidence-log observe `WriteRow(...)`
  failures; and the runtime emits rate-limited
  `runtime_logging.csv_write_failed` / `runtime_logging.csv_write_recovered`
  events. Verified by `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` on
  2026-06-20. Slice B added sticky `logging_health.json` and health degradation
  for event-log append failure. The status/snapshot retry slice added sticky
  status/snapshot publish failure/recovery events, prompt retry after failed
  control status publication, and control snapshot retry timing that advances
  only after a successful write. Slice C added `analyze report` consistency
  diagnostics for manifest-declared, archive-ingested, and latest-mirror CSV row
  count disagreement: running sessions warn, closed runs are suspect evidence.
  Keep CSV byte-cap retention separate unless later evidence ties it directly to
  evidence loss.

### Machine profiles (FEAT-0023) — contract-doc updates — DONE 2026-06-21

FEAT-0023's Change-Checklist documentation, which shipped in code but not in the
contract docs, was completed 2026-06-21 (source-grounded names taken from
`runtime_csv_rows.cpp` / `runtime_status.cpp` / `runtime_supervisor_state.cpp` /
`runtime_lifecycle.cpp` / `control_supervisor.cpp`, not spec placeholders):

- `docs/RUNTIME_HOME.md` — `profile.switch.request.json` + `profile.cycle.request.json`
  in the Files inventory with dedicated subsections; the optional read-only
  `machine_id.txt` input (framed as a non-Control-owned input); `active_profile_name`
  / `active_profile_source` in the read-loop and control-loop (v4) status field lists
  and `active_profile_name` in `control_supervisor.json` (each with an additive-absence
  note); the two active-profile control-loop CSV columns; the five
  `supervisor.profile_*` events.
- `docs/CONTROL_LOOP.md` — new "Profile Switch (FEAT-0023)" section (validate-before-
  activate, the `profile.cycle.request.json` signal, graceful-restore restart,
  no-backoff respawn, `kSwitchCycleTimeout` force-terminate fallback, auto-revert).
- `docs/WRITE_ORCHESTRATION.md` — the switch reuses the existing graceful
  shutdown-restore (fans → captured BIOS baseline during the ~1–2 s gap); the
  restore is skipped only on a missed cycle deadline (force-terminate).
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` — the two active-profile CSV columns
  (control-loop CSV only, not analyzer-ingested).
- `docs/CODE_MAP.md` + `docs/STRUCTURE_AND_STABILITY.md` — the four new modules
  (`machine_profile`, `profile_composition`, `profile_switch_decision`,
  `machine_identity`), `runtime_lifecycle` request-file lifecycle, the extended
  `control_supervisor` entry, and the `config/overlays` / `config/profiles` artifacts.

The on-hardware live M (`REQ-MPROFILE-10`) remains the only open FEAT-0023 item.

### `--set-profile` CLI mutual-exclusion guard (FEAT-0023 follow-up)

`--set-profile <name>` (FEAT-0023; parsed at `app_args.cpp:157`) has no
mutual-exclusion guard and is dispatched in `app_main.cpp` (~`:255`) after the
read-only runtime commands (`--show-config`, `--service-probe`, `--health`,
`--status`), each of which returns first. So `svg-mb-control --set-profile X
--status` (or `--health` / `--show-config` / `--service-probe`) silently runs the
read-only command and never writes `profile.switch.request.json`, with no error —
unlike the sibling `--reset-breakers`, which is guarded at `app_main.cpp:185-192`
("cannot be combined with another runtime command"). An empty `--set-profile` name
is already rejected (`runtime_lifecycle.cpp:123-125`). This is a CLI-robustness
gap, not a safety issue; a fix changes CLI behavior and must go through the Feature
Intake Gate (`AGENTS.md`). Record-only for now.

### Runtime disk growth (FEAT-0015 / FEAT-0016) — implemented 2026-06-18

The runtime home was on a disk-fill path after FEAT-0020 widened the standard
control-loop CSV and enabled live energy logging. The immediate operational
reclaim is done: the derived analyzer DB
`release\runtime\svg_mb_control.db` was deleted on 2026-06-18, reclaiming
8,377,511,936 bytes (7.80 GiB). Raw CSV/event files and live sidecars were left
untouched; `release\runtime` measured 3.439 GiB after the reclaim.

The structural work is implemented and verified by
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir` on 2026-06-18:

- **FEAT-0016** (`REQ-DBRETAIN-*`) — DB-side retention is part of `analyze prune`
  (`--db-retain-days`), cascade-delete old `runs` under `foreign_keys=ON`, and
  reclaim with a post-delete `VACUUM`. This closes the highest disk-growth item
  because the DB had reached 7.80 GiB and is a designed derived sink.
- **FEAT-0015** (`REQ-EVENTRET-*`) — event JSONL rotation reuses
  `log_rotate_hours` / `log_retain_days`, and routine
  `control_loop.write_applied` events are sampled while diagnostic/lifecycle
  events remain persisted within the retained window. The live event JSONL was
  270.3 MB on 2026-06-18; the logs directory was 3.265 GiB.

### Require fresh runtime evidence before changing cadence/floor defaults
The shipped profile is a `250 ms` control tick and `250 ms` write cooldown. Per
`README.md` (Documentation) and `docs\RUNTIME_LOGGING_AND_EVALUATION.md`, lowering
cadence, enabling a floor below the shipped profile, adding live channels, or
broader strategy changes must be backed by fresh measurement evidence before the
default changes.

The mechanism is the evaluation workflow, not `Test-LocalCI.ps1`:
`Test-LocalCI.ps1` runs `Build-Release.ps1 -NoStopProcesses -NoPublish` (a
hermetic build plus unit/smoke lane) and produces no runtime CSV. Collect runtime
evidence instead with a live control-loop capture followed by
`svg-mb-control analyze ingest` + `analyze report`, and/or
`scripts\Compare-CpuTemps.ps1` for the CPU-temperature-by-load view
(`docs\cpu-temp-comparison-harness.md`). When package-power fields are present,
also run `scripts\analyze_cpu_temp_power.py` so CPU temperature is compared
against actual heat input, not busy percent alone.

### FEAT-0006 (CPU work/energy) downstream work
The spec is `Accepted` (`docs\features\FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
v0.7, 2026-06-21). The package-energy and cycle slices are landed, the all-core
package effective-frequency rollup is merged (PR #25, analyze schema v13), and the
section-12 loop-timing gate harness is merged (PR #26); the open work is now
evidence/promotion, not new code. Governed by FEAT-0006 and the `REQ-CPUEFF-*`
rows in `docs\TRACEABILITY.md`.

- Analyzer average-watts report derived from `cpu_pkg_energy_delta_uj` /
  `cpu_power_window_ms` over distinct `cpu_power_sample_id` (average power is not
  logged; it is derived).
- Live evidence for the *enabled* RAPL path
  (`SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`): **ran live on hardware 2026-06-18**
  via the FEAT-0020 flip — `cpu_pkg_energy_acquisition=quarantine`, `cpu_power_*`
  populating, analyzer `package_power` avg 86.74 W (see
  `docs/feat-0020-live-flip-validation-results-2026-06-18.md`). The later
  standard-loop snapshot captured 1228 package-energy windows with CPU and GPU
  watts beside temperatures (`docs/power-temp-comparison-snapshot-2026-06-18.md`).
  The marker stays `quarantine` (the `quarantine → validated` Evaluation gate
  below is unchanged).
- The `quarantine` → `validated` Evaluation gate for `cpu_pkg_energy_acquisition`
  (this is never set automatically).
- Cycle / effective-frequency path: **landed.** The per-core APERF/MPERF logger
  + analyzer effective-frequency derivation (analyze schema v10) landed
  2026-06-09/10, and the all-core **package** rollup via a dedicated off-thread
  sweeper merged 2026-06-21 (PR #25, analyze schema v13); the consumers
  (`scripts\score_energy_session.py` criterion-4 and
  `scripts\cpu_config_fingerprint.py`) prefer the package columns with a per-core
  fallback. The work numerator (ΔAPERF) and effective frequency are reachable with
  the shipped bin (the 2026-06-07 `#GP` was a probe-index error, corrected
  2026-06-09). **§12 loop-timing gate: ran live 2026-06-25 — PASS.** Three
  attended captures (two cycles-OFF baselines + one cycles-ON candidate, 28-thread
  load) show the off-thread sweeper does not move the 250 ms profile: ON
  `loop_work_duration_ms` p99-bulk 72.15 ms < both OFF baselines (86.28 / 81.56),
  0 buckets moved (default + calibrated), CPU-phase-split idle p50 2.779 ms lowest
  of three; sweeper confirmed running (5084/5084 rows × 32 cores); 6-skeptic
  adversarial verify all `pass_holds`
  (`docs\feat-0006-loop-timing-gate-evidence-2026-06-25.md`). **Marker decision
  recorded 2026-06-25: the cycle/all-core acquisition marker (`cpu_cycles_acquisition`)
  is promoted `quarantine → validated`** (governance decision in
  `docs\cpu-work-energy-acquisition-decision-2026-06-07.md` §Quarantine-exit
  decision; recorded outcome, logged marker stays `quarantine`, analyzer does not
  branch). Remaining (non-blocking):
  - Optional Option-B locked-clock criterion-4 cross-check (validates the derived
    all-core idle 5339 / load 5278 MHz @ P0 4300 against a locked setpoint and the
    P0 base) — a future strengthening, not a promotion blocker (decision-doc §4 was
    met by plausibility + affinity stability).
  - The cycles-per-Joule energy↔cycle join (the two paths carry separate sample
    ids and no join rule is specified; options doc 2026-06-16 recommends Option C).
  - Non-blocking gate-tooling follow-up: `score_loop_timing_gate.py` is coarse
    (p99-bulk only, ~10 ms MDE; GPU bucketing inert under GPU-idle). The wall-clock
    CPU-phase split + median is the sensitive analysis and could be folded into the
    gate tooling or evidence procedure. The 2026-06-25 evidence used n=2 baselines
    (one off-vs-off spread estimate); more baselines would harden the threshold,
    and a GPU-busy capture would extend coverage beyond the GPU-idle regime.

### FEAT-0008 (watchdog hung-worker recovery) post-v1 only

FEAT-0008 v1 is implemented and verified: the `--restart` stop-timeout path now
escalates to a bounded force-terminate, automated C++/Python tests pass, and the
live deterministic suspend measurement verified the production watchdog recovery
mechanism. There is no remaining v1 recovery-path evidence to collect.

Remaining items are post-v1 options or hardening only, governed by
`docs\features\FEAT-0008-watchdog-hung-worker-recovery.md` §11:

- Make the force-kill grace period configurable if the fixed 15 s deadline proves
  wrong in practice.
- Consider applying the same escalation to plain `--stop` after timeout.
- Consider a fail-closed recovery fallback for the pre-first-write PID
  corroboration race.
- Consider canonicalizing image paths if a future install uses `subst`, mapped
  drives, junctions, or symlinked `release\` paths.

### Include ownership — decision-gated, no action yet
`docs\STRUCTURE_AND_STABILITY.md` already records converting `#include` directives
to module-qualified paths "only if the team wants stricter include ownership; the
current build keeps compatibility include roots" (`SVG_MB_CONTROL_INCLUDE_DIRS` in
`CMakeLists.txt`). This is a team decision, not a standalone next step; do nothing
until that decision is made.

### Docs archive / merge / compact pass

Partly completed 2026-06-09:

- **Done** — archived dead point-in-time snapshots under `docs\archive\`:
  `build-optimization-results.md`, `code-quality-pass-2026-05-19.md`,
  `evaluation-and-optimization-recommendations.md`.
- **Done** — merged the five overlapping Bench-vs-Control logging notes into
  `docs\bench-logging-history.md`; the archived originals remain under
  `docs\archive\` for audit history.

Current state:

- **Done 2026-06-20** — compacted and archived
  `docs\archive\modular-profile-hotswap-discussion-2026-06-06.md` into a short
  held-design pointer; compacted
  `docs\archive\modular-profile-hotswap-plan-2026-06-06.md` while preserving the
  current-law and reusable-primitive tables; compacted
  `docs\archive\profile-hot-swap-allow-live-decision-2026-06-06.md` into a
  support pointer to revised D6; folded the remaining guidance from four closed
  redirect stubs (control optimization, dashboard health polling, next logging
  targets, and GPU response refinement) into the current docs/backlog and deleted
  the stubs.
- **Done 2026-06-20** — moved completed implementation plans and the executed
  FEAT-0006 live-validation procedure into `docs\archive\implemented-plans\`:
  the completed campaign plan, FEAT-0019 sidecar hot-path plan, FEAT-0020
  critical-evaluation/implementation plans, and the FEAT-0006 live MSR
  validation plan. Also moved the applied testing/hot-path simplification review
  and the older closed simplification/logging/script-stack target records there.
- **Done 2026-06-20** — moved bulky historical evidence/planning records out of
  root `docs\`: the two-pass discovery-loop fan-out/checkpoint set, the parked
  CPU power-anticipation scaffolds and Gate-2 measurement record now live under
  `docs\archive\`; the FEAT-0008 incident-only evidence records were removed.
- **Remaining** — finish the CPU work/energy consolidation by folding the
  resolved `cpu-cycle-counter-source-decision` into
  `cpu-work-energy-acquisition-decision`, and then decide whether the older
  feasibility/results pair should stay as separate evidence records. Preserve
  the live forward-gates: quarantine-exit Evaluation, off-thread sweeper
  M-evidence/promotion, and the open 400 W ceiling.
- **Keep as-is** (load-bearing or open work): `adaptive-cadence-design`
  (CONTROL_LOOP), `discovery-polling-logging-state` + `discovery-steady-response-control`
  (MEASUREMENT_GATE), `discovery-control-math-performance`
  (`docs\archive\implemented-plans\CONTROL_SIMPLIFICATION_TARGETS.md`),
  `discovery-recovery-gap-audit-2026-06-04`
  (open remediations 1 & 2), `testing-harness-evaluation-2026-06-06` (open §5
  coverage gaps), and `discovery-gpu-temp-envelope` (sole record of the RTX 5090
  zero-hotspot envelope constraint — lift that fact into `CONTROL_PIPELINE_MATH.md`
  or `COOLING_STRATEGY.md` before removing it, if ever).
- **Done 2026-06-20** — refreshed `CODE_MAP.md`'s "## Docs" taxonomy for
  feature specs, traceability, decision records, current operational/evaluation
  records, compacted implementation records, and historical/discovery stubs.

Content-drift items from the same assessment were fixed 2026-06-09:

- `docs\source-aware-blend-decision-2026-05-26.md` now states that the
  channels `1`/`5` `cpu_only` claim was true for the 2026-05-26 deployment and
  later superseded by the 2026-06-09 radiator-exhaust GPU response decision.
- `docs\response-evaluation-tuning-plan.md` now distinguishes the deployed
  `release\control.json` baseline from the pending, not-yet-deployed 2026-06-09
  repo-config GPU retune.

### Optional housekeeping — low priority, defer
- Archiving closed implementation records (`CONTROL_SIMPLIFICATION_TARGETS`,
  `LOGGING_IMPROVEMENT_PLAN`, `SCRIPT_STACK_REVIEW`): done 2026-06-20 under
  `docs\archive\implemented-plans\` after the flat layout became noisy.
- Splitting the 17 flat Python tests in `tests\` into mode subdirectories: the
  C++ lane is already isolated under `tests\cpp\`. A split risks the shared
  `tests\helpers.py` imports and the `python -m unittest discover tests` pattern;
  defer until the suite is large enough to justify the per-subdirectory
  `__init__.py` work.

## Prior list — corrected status

The earlier list contained seven items. Verified against the repo:

- **Add `-NoStopProcesses` to `Test-LocalCI.ps1`** — already done.
  `Test-LocalCI.ps1` already invokes `Build-Release.ps1 -NoStopProcesses
  -NoPublish` and is documented as running without stopping a live controller.
- **Promote FEAT-0006 Draft → Accepted** — stale. FEAT-0006 is already
  `Accepted` (v0.7, 2026-06-21). The real work is the evidence/promotion items
  above.
- **Validate cadence via `Test-LocalCI.ps1` against fresh runtime CSV** — wrong
  tool. The intent is correct (see "Require fresh runtime evidence" above), but
  `Test-LocalCI.ps1` produces no runtime CSV; use the analyze workflow / the
  comparison harness.
- **Maintain traceability** — not a discrete step; it restates a standing
  `AGENTS.md` rule (Feature Intake Gate, Change Checklist).
- **Enforce strict includes** — already captured as an optional, decision-gated
  item in `docs\STRUCTURE_AND_STABILITY.md` (see above).
- **Archive closed plans** — substantially done 2026-06-20; keep only
  load-bearing/open plans at the root (see above).
- **Organize Python tests** — optional housekeeping, deferred (see above).
