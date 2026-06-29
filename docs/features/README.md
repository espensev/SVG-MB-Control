# svg-mb-control — Feature specs

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.6   **Updated:** 2026-06-22
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/MEASUREMENT_GATE.md`
**Purpose:** define the *spec-before-build* system — each new feature gets its
own detailed spec **before** it is implemented, so the implementation can be
checked against the written spec afterward.

## Current priority

Read-first index of in-flight and next feature work. This is the priority view
for agents and maintainers; the §5 registry stays authoritative for per-feature
status and `git log` for what shipped. `docs/next_steps.md` is retained as
topical background, not as a second priority queue. Keep this block current when
a feature's status or the decision queue changes. A fuller standing review is
`docs/archive/spec-and-backlog-structure-assessment-2026-06-18.md`.

**Recently implemented:**

- **FEAT-0021** (`Implemented`, T/R verified; live M PASS-with-finding 2026-06-25) — standard
  control-loop GPU workload context logging beside FEAT-0020 GPU power. The
  control CSV now includes a cached, additive context slice for utilization,
  pstate, graphics/memory clocks, VRAM used/total, and sample identity/time/age.
  Analyzer schema v12 ingests the optional fields and reports context summaries
  only when present; older archives degrade as unavailable.
- **FEAT-0022** (`Implemented`) — runtime logging failure
  visibility for CSV/archive/mirror/manifest sink failures and event-log append
  failure. `WriteRow(...)` failures are now observed by control-loop,
  read-loop, and evidence-log; failures/recoveries emit rate-limited
  `runtime_logging.csv_write_*` events. Event-log append failure writes sticky
  `logging_health.json` and degrades health while active. Status/snapshot
  publish failures now emit sticky failure/recovery events and retry promptly.
  Analyzer reports now classify running CSV manifest/archive/latest-mirror
  row-count mismatch as a warning and closed mismatch as suspect evidence.
- **FEAT-0016** (`Implemented`) — analyze SQLite DB retention. Immediate safe
  reclaim was completed 2026-06-18 by deleting the derived
  `release/runtime/svg_mb_control.db` (7.80 GiB reclaimed); the product-code work
  now adds `analyze prune --db-retain-days` run-purge plus post-delete reclaim.
- **FEAT-0015** (`Implemented`) — runtime event JSONL retention. Event logs now
  rotate on the runtime retention window and sample routine
  `control_loop.write_applied` while diagnostic/lifecycle events remain within
  the retained window.
- **FEAT-0019** (`Implemented`) — sidecar persistence off the actuation hot path.
  D-WRITEHOT-1 is Current; identity-gated persistence and flush-side reset
  behavior are covered by C++ tests and traceability rows.
- **FEAT-0004** (`Done`) —
  hardware-access dependency health signal for PawnIO availability. Runtime
  status/health now exposes additive tri-state `hwaccess_*` fields for AMD/SMN
  read-path and Super I/O write-path initialization outcomes, and startup emits
  `control_loop.hwaccess_*` / `read_loop.hwaccess_*` transition events. Live M
  evidence 2026-06-22 captured `read_loop.hwaccess_restored` from an isolated
  read-loop run. Exit codes and driver-management behavior are unchanged.

**Recently implemented (continued):**

- **FEAT-0023** (`Implemented` 2026-06-21; commits
  `0952e3d`/`1195d84`/`9a78a11`/`79145e4`/`e431dfd`/`fb70be5`) — machine profiles
  + restart-based profile switch. Startup resolution
  (`--profile`/`SVG_MB_PROFILE`/machine-id/default, `config/profiles/<name>.json`)
  + the live switch (`--set-profile`; supervisor validates, gracefully cycles the
  worker accepting the BIOS-auto gap, no-backoff respawn, auto-revert) +
  machine-base/overlay composition (`ComposeConfigRoot`; ships
  `config/overlays/release.behavior.json` + `config/profiles/snd-desk-composed.json`,
  proven to reproduce `control.release.json`) + active-profile CSV/status fields +
  a revert integration test. The 2026-06-22 Rust local UI helper
  (`tools/profile_switch_ui`) wraps the existing status/health/`--set-profile`
  CLI path without adding runtime semantics. The on-hardware live M (REQ-MPROFILE-10)
  was **closed PASS 2026-06-25** via Option A2 (temporary task `--config` repoint to
  the composed default, reverted): the live worker ran `active_profile_name=snd-desk-composed`
  healthy with the resolved control config byte-identical to `release\control.json`
  on hardware (`docs/feat-0023-live-default-profile-evidence-2026-06-25.md`).
  Productizing the catalog into `release\` (Option B) is the remaining optional
  follow-up.

**Active — `Accepted`, buildable when implementation is authorized:**

- **FEAT-0006** (`Accepted`) — CPU work/energy efficiency evidence. The
  package-energy + cycle slices are landed; the all-core **package**
  effective-frequency rollup (off-thread sweeper, analyze schema v13) and its
  section-12 loop-timing gate harness merged 2026-06-21 (PRs #25/#26). The **§12
  off-thread-sweeper loop-timing gate ran live 2026-06-25 — PASS** (sweeper does
  not move the 250 ms profile; ON p99-bulk 72.15 < OFF 86.28/81.56; 6-skeptic
  adversarial verify all `pass_holds`; `docs/feat-0006-loop-timing-gate-evidence-2026-06-25.md`),
  clearing the §12 precondition. The cycle/all-core acquisition marker
  (`cpu_cycles_acquisition`) was then promoted **`quarantine → validated` by
  governance decision 2026-06-25** (`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`
  §Quarantine-exit decision; recorded outcome, not a code/CSV change). Remaining is
  non-blocking: the cycles-per-Joule join, an optional Option-B locked-clock
  criterion-4 cross-check, and the deferred CPU-setting label. See FEAT-0006 §14 and
  `docs/next_steps.md` (FEAT-0006 downstream work).
- **FEAT-0001** (`Accepted`) — hot-swap write policy. Spec accepted; not yet
  implemented; build when authorized.
- **FEAT-0005** (`Accepted`) — write actuation confirmation, Phase-1 RPM-based
  detection/evidence. Decision record is current; build when authorized.

**Decisions or gates owed (not buildable until cleared):**

| Spec | What it owes (source section) |
|---|---|
| **FEAT-0014** (held Draft) | §11: where the reconcile/restore blocked-channel guard lives (Control-layer pre-check only vs also mirror `channel_blocked` into the vendored restore); whether a skipped entry is cleared or retained; whether `--write-once`'s exit-5 refusal suffices. Not reachable under the shipped single-profile config; promote only if a multi-profile config makes it reachable. |
| **FEAT-0025** (new Draft 2026-06-22) | §9: promote D-AMDGPU-1 (`docs/amd-gpu-telemetry-decision-2026-06-22.md`) from Proposed to Current — confirm AMD ADLX (read-only, vendored, dynamic-load) over legacy ADL, and the logging-only-first ship order. §12: loop-timing impact + on-hardware AMD-temperature-validity evidence owed before AMD temperature may drive the GPU envelope. Read-only AMD (Radeon) GPU support so the GPU envelope and FEAT-0020/0021 logging slices work on an AMD GPU machine. |
| **FEAT-0009** (held Draft) | §12 measurement gate: run the A/B contention experiment to justify promotion (the default is already `inherit`). §11 also holds two open choices (`above_normal` vs `high_timecritical`; hot-reloadable vs startup-only). Not in `docs/next_steps.md`. |

The two latency held-Drafts also owe gate decisions and live in
`docs/next_steps.md` (latency-reduction section): **FEAT-0017** owes the
lanes/target-ceiling decision plus a response-evaluation Pass-3, and **FEAT-0018**
owes the floor characterization pass (it crosses `docs/MEASUREMENT_GATE.md`).

- **FEAT-0024** (`Draft`) — intake-lead fan response under load. Config-only
  surge-and-hold retune of the intake lanes (`2`/`3`/`4`): joint rise-rate +
  step-cap raise (largest on the slowest lane `4`), intake-first `gpu_airflow`
  onset, and a steeper intake `cpu_override` mid-band, so the intakes supply
  airflow ahead of the exhausts under load. Idle is out of scope and unchanged;
  rise-asymmetric (spin-down not made faster). Direction settled in
  `docs/intake-lead-response-decision-2026-06-25.md`; owes the candidate-magnitude
  selection and a response-evaluation Pass-1/Pass-3 validation before promotion.

**`Done` 2026-06-22:** **FEAT-0003** —
restart-selected control-law profile seam (PID / P / PI / PD plus the current
curve law). Built across slices F3-1..F3-5: the `IChannelController` seam +
`CurveOverlayController` (output-identical, forward-wrap) + `PidController`
(shadow/dry-run by default) + per-channel `controller`/`pid` config dispatch +
the decision-D6 measurement-gate gate (downgrade-to-shadow) + kind-aware CSV/
status reporting. Hardened per a 5-lens adversarial review (slew-cap bypass,
NaN-integral guard, kind-aware status nulls). Spec §14 + `TRACEABILITY.md` filled;
PID identity in `docs/CONTROL_PID_MATH.md`. The first shadow-replay
characterization artifact is
`docs/pid-shadow-characterization-2026-06-21.md`; it rejects all-channel live PID
and leaves only a channel-0-only experiment as a possible operator-gated pass.
That **live PID write on hardware** gate (REQ-PROFILE-07 M) passed 2026-06-22 in
`docs/pid-live-channel0-evidence-2026-06-22.md` with `pid.allow_live`, the
persisted characterization artifact, and the positive slew cap in place; the
package rolled back to the shipped `curve_overlay` default afterward. REQ-PROFILE-05
is a deliberate functional-pass partial (curve state stays on `ChannelState`
under the restart-selected scope).

**Recently shipped (context — see `git log` / `docs/next_steps.md`):** the
write-path safety review (FEAT-0010/0011/0012/0013) closed 2026-06-17 —
`Implemented` and merged; FEAT-0014 is the one non-blocking remainder. FEAT-0020
standard control-loop power logging shipped 2026-06-18 — `Implemented` v0.4,
live flip executed and validated (gate 6 closed; CPU package + GPU board power
now log on the live control loop). The flip also produced the first enabled-RAPL
on-hardware evidence FEAT-0006 was waiting for (`package_power` avg 86.74 W
live). The later `docs/power-temp-comparison-snapshot-2026-06-18.md` preserves
the standard-loop CPU+GPU watts beside temperatures for future comparisons.
FEAT-0021 followed on 2026-06-20 with cached GPU workload context in the same
standard CSV and analyzer v12 support; live cadence M evidence remains the
deployment check for that added periodic context read.

**Background only:** `docs/next_steps.md` and `docs/PATH_NOTES.md` preserve
topic history; archived discovery/evidence records under `docs/archive/` are not
implementation queues. Promote useful findings through this feature registry
before product-code work starts.

## 1. What this is

This folder holds one **feature spec** per planned feature, written from
[`_FEATURE_TEMPLATE.md`](_FEATURE_TEMPLATE.md). A feature spec is the durable
contract for a feature: what problem it solves (from observed runtime evidence or
a named code/contract gap), the exact behavior, its requirements (`REQ-*` IDs),
config/schema deltas, the design decision(s) it depends on, and — critically —
its **acceptance criteria and a verification log** so that after the feature is
built you can confirm it against what was promised.

This does not replace the existing repo contracts; it threads them together.
`AGENTS.md` stays the canonical agent contract (`CLAUDE.md`); a feature spec is
where a single new feature is scoped against those contracts before code lands.

| Surface | Role | Relationship to a feature spec |
|---|---|---|
| `AGENTS.md` | Canonical build / boundary / safety / change-checklist contract. | The rules every feature spec is scoped against. |
| `docs/STRUCTURE_AND_STABILITY.md` | Source layout and responsibility boundaries. | Says *where* a feature's code lives. |
| `docs/CONTROL_LOOP.md`, `READ_LOOP.md`, `WRITE_ORCHESTRATION.md`, `RUNTIME_HOME.md`, `CONTROL_PIPELINE_MATH.md` | Mode and runtime behavior contracts. | The behavior sections a spec references instead of restating. |
| `docs/MEASUREMENT_GATE.md` | What is blocked until characterized (cadence, channels, mixed-input strategy). | A spec must declare whether it crosses the gate and what evidence it needs. |
| `docs/<topic>-decision-YYYY-MM-DD.md` | One dated decision per direction-setting choice. | A feature earns its decision record before implementation; the spec links it. |
| `docs/TRACEABILITY.md` | Central `REQ-*` to verification map. | Every feature requirement gets one audit row here when introduced or changed. |
| `docs/FEATURE_VERIFICATION_CHECKLIST.md` | Practical implementation and handoff checklist. | The checklist used while building and verifying an accepted feature. |
| `.\scripts\Test-LocalCI.ps1` / `.\build-release.ps1` | CI-style validation and release gates. | Where a spec's `REQ-*` requirements are verified. |
| `tests/test_feature_specs.py` | Machine check for feature-spec consistency. | Fails the Python lane if registry, requirement, traceability, gate, or verification-log state drifts. |

> A `Reserved` or `Draft` spec is planning, not a backlog to start, and not
> permission to write code. Nothing in a feature spec is normative until its
> promotion gates (§3) pass and implementation is authorized.

## 2. Lifecycle of a feature spec

```text
Reserved  ->  Draft  ->  Accepted  ->  Implemented  ->  Done
```

1. **Reserved** — listed in the registry below with a `REQ-*` namespace, but the
   spec body is not yet written (or is a stub).
2. **Draft** — the spec is being written in detail (this is "document it before
   we add it"). The promotion gates (§3) are worked through here.
3. **Accepted** — the spec is agreed and may become buildable work when
   implementation is explicitly authorized; its required design decision
   record(s) exist and are current; `REQ-*` IDs are assigned; verification is
   mapped to real checks (`Test-LocalCI`, build-release, contract review, or
   runtime evidence) and to `docs/TRACEABILITY.md`.
4. **Implemented** — code lands; the relevant docs are updated per `AGENTS.md`
   §Change Checklist. The spec's **verification log** is filled in: each `REQ-*`
   is checked against the running controller ("the check-against-it-later step").
5. **Done** — all acceptance criteria pass; the spec is the historical record of
   what shipped.

A spec may sit at **Draft with all promotion gates (§3) checked** when it is
complete design-capture that is sequenced behind another feature or not yet being
promoted. That differs from **Accepted**: Accepted means agreed and authorizable
(still needing explicit build authorization unless the spec says otherwise),
whereas a held/sequenced Draft is complete design that is not buildable yet.

## 3. Promotion gates

Before a feature becomes buildable work, its spec must clear all gates in the
template's §13 checklist. In short:

1. Problem statement sourced from observed runtime evidence or a named
   code/contract gap — not speculation.
2. Identify which repo invariant it stresses: Repo Boundary and Live Runtime
   Safety (`AGENTS.md`), the Measurement Gate baseline (`MEASUREMENT_GATE.md`),
   control-computation identity (`CONTROL_PIPELINE_MATH.md`), and runtime-home
   schema stability (`RUNTIME_HOME.md`).
3. Write or update the design decision record(s) **before** implementation
   starts (dated `docs/<topic>-decision-YYYY-MM-DD.md`).
4. Assign concrete `REQ-*` IDs only after the decision chooses a direction.
5. Map every requirement to a real verification check in the feature spec and
   in `docs/TRACEABILITY.md` in the same change that introduces it.
6. Confirm it does not violate Live Runtime Safety or Repo Boundary and does not
   silently move the Measurement Gate baseline.
7. Doctrine check (`CLAUDE.md`): grounded claims, correct `must`/`should`/`is`
   usage, no undefined terms or unqualified vague adjectives.

## 4. Naming

- One file per feature: `FEAT-NNNN-<slug>.md` (e.g. `FEAT-0001-<slug>.md`).
- `NNNN` is a zero-padded, never-reused sequence number.
- H1 is the ID: `# FEAT-NNNN: <Feature name>`.
- Each feature owns exactly one `REQ-<AREA>-*` namespace, reserved in §5 below.
  This registry is the namespace authority for the repo.

## 5. Feature registry

Reserve a row (FEAT id + `REQ-*` namespace) when a feature is first proposed, so
it has a home before it is built. Write the spec body when implementation
approaches. None below is normative until its promotion gates pass. A `Reserved`
row carries no linked spec body in the enforced set; a body written then deferred
is parked under [`_parked/`](_parked/) and the row rejoins the enforced set
(`tests/test_feature_specs.py`) only when promoted back to `Draft`.

| FEAT | Feature | `REQ-*` namespace | Status |
|---|---|---|---|
| [FEAT-0001](FEAT-0001-hot-swap-write-policy.md) | Hot-swap runtime write policy | `REQ-WRITEPOLICY-*` | Accepted |
| [FEAT-0002](FEAT-0002-cpu-settings-evidence-logger.md) | CPU settings evidence logger | `REQ-CPUSETTINGS-*` | Implemented (source/test load layer; label deferred) |
| [FEAT-0003](FEAT-0003-selectable-profile-hot-swap.md) | Restart-selected control-law profile seam | `REQ-PROFILE-*` | Done (2026-06-22; seam + curve-overlay + PID shadow/dry-run + config dispatch + D6 gate + kind-aware reporting; REQ-PROFILE-05 partial; channel-0 live PID M passed; default remains curve) |
| [FEAT-0004](FEAT-0004-hardware-access-health-signal.md) | Hardware-access dependency health signal (PawnIO availability) | `REQ-HWHEALTH-*` | Done (2026-06-22; additive `hwaccess_*` status/health fields + startup transition events; live M event-log evidence captured; exit codes unchanged) |
| [FEAT-0005](FEAT-0005-write-actuation-confirmation.md) | Write actuation confirmation (non-actuating-write detection) | `REQ-ACTCONFIRM-*` | Accepted |
| [FEAT-0006](FEAT-0006-cpu-work-energy-efficiency-evidence.md) | CPU work & energy efficiency evidence (work-per-Joule) | `REQ-CPUEFF-*` | Accepted (energy logger + analyzer avg-power landed; cycle APERF/MPERF logger + analyzer effective-frequency derivation landed 2026-06-09/10; all-core package rollup via off-thread sweeper + section-12 loop-timing gate harness merged 2026-06-21, analyze schema v13; energy quarantine-exit evidence complete across 3 independent sessions; marker promotion + off-thread-sweeper live M-evidence remain) |
| FEAT-0007 | RAM temperature telemetry (per-DIMM, via existing Super I/O read) | `REQ-RAMTEMP-*` | Reserved (body parked in `_parked/`) |
| [FEAT-0008](FEAT-0008-watchdog-hung-worker-recovery.md) | Watchdog hung-worker recovery (force-kill escalation) | `REQ-WATCHDOG-*` | Done (v1 complete: force-terminate escalation + `--restart` wiring landed; C++ unit + Python suspend-based integration tests pass; live deterministic suspend recovery evidence passed; post-v1 options remain in FEAT-0008 §11) |
| [FEAT-0009](FEAT-0009-controller-priority-elevation.md) | Controller scheduling-priority elevation (worker priority raise + recovery co-elevation) | `REQ-PRIORITY-*` | Draft (held — pending the FEAT-0009 §12 A/B contention experiment) |
| [FEAT-0010](FEAT-0010-write-actuation-sidecar-fault.md) | Write actuation survives a sidecar-persistence fault (sidecar persist failure must not veto the fan write) | `REQ-WRITESAFE-*` | Done (2026-06-17; runtime-reproduced H1; veto removed + additive persist-failure counter, C++ tests green) |
| [FEAT-0011](FEAT-0011-write-failure-breaker-rising-cooling-demand.md) | Write-failure breaker must not block rising cooling demand (recovered-actuator self-heal in the cooling direction) | `REQ-COOLWRITE-*` | Done (2026-06-17; bounded half-open probe — one write per 5 s on rising cooling demand closes a recovered breaker; C++ tests green) |
| [FEAT-0012](FEAT-0012-startup-tolerates-corrupt-pending-writes-sidecar.md) | Startup quarantines a corrupt `pending_writes.json` and proceeds as empty instead of fatally aborting the worker into a relaunch-thrash loop | `REQ-SIDECARRESIL-*` | Done (2026-06-17; Direction A — quarantine + proceed + event + degraded health; C++ + smoke tests green) |
| [FEAT-0013](FEAT-0013-source-aware-primary-dropout-safe-mode.md) | Source-aware channels enter safe mode on primary-source dropout (a CPU dropout on a max-blend channel now trips the existing safe-mode mechanism instead of being masked by GPU) | `REQ-SRCSAFE-*` | Done (2026-06-17; reuses the 3-miss sensor-failure trip; C++ tests green) |
| [FEAT-0014](FEAT-0014-reconcile-restore-blocked-channel-guard.md) | Reconcile/restore honor the runtime blocked-channel write policy | `REQ-RESTOREGUARD-*` | Draft (held — real restore-path gap, not reachable by the shipped single-profile config; pending maintainer direction) |
| [FEAT-0015](FEAT-0015-event-log-retention.md) | Event JSONL has a retention bound | `REQ-EVENTRET-*` | Implemented (2026-06-18; rotation + write-applied sampling; Test-LocalCI green) |
| [FEAT-0016](FEAT-0016-analyze-db-run-purge.md) | Analyze SQLite DB has a retention bound | `REQ-DBRETAIN-*` | Implemented (2026-06-18; `analyze prune --db-retain-days`, cascade purge + reclaim; Test-LocalCI green) |
| [FEAT-0017](FEAT-0017-faster-fan-reaction-under-load.md) | Faster fan reaction under load (control-response retune: joint rise-rate + step-cap raise, asymmetric, lane-targeted) | `REQ-REACT-*` | Draft (held — lanes/target ceiling undecided; pending a response-evaluation Pass-3 validation) |
| [FEAT-0018](FEAT-0018-adaptive-cadence-enablement.md) | Adaptive-cadence enablement under thermal transient (engage the dormant `poll_tick_floor_ms` engine) | `REQ-CADENCE-*` | Draft (held — crosses the measurement gate; pending the floor characterization pass) |
| [FEAT-0019](FEAT-0019-sidecar-persist-off-hot-path.md) | Sidecar persistence off the actuation hot path (identity-gated `Persist()`) | `REQ-WRITEHOT-*` | Implemented (2026-06-18; T/R verified, C++ tests green) |
| [FEAT-0020](FEAT-0020-standard-control-loop-power-logging.md) | Standard control-loop power logging (CPU package energy + GPU power in the same control-loop CSV, logging-only) | `REQ-PWRLOG-*` | Implemented (2026-06-18; T/B/R/M verified, full Test-LocalCI green; per-tick 5-field GPU power slice; live flip deployed + validated, gate 6 closed) |
| [FEAT-0021](FEAT-0021-standard-control-loop-gpu-workload-context-logging.md) | Standard control-loop GPU workload context logging (utilization, clocks, pstate, and VRAM beside GPU power, logging-only) | `REQ-GPUCTX-*` | Implemented (2026-06-20; cached 1000 ms context sample, analyzer schema v12, T/R verified; REQ-GPUCTX-04 live M PASS-with-finding 2026-06-25, `docs/feat-0021-live-cadence-evidence-2026-06-25.md`) |
| [FEAT-0022](FEAT-0022-runtime-logging-failure-visibility.md) | Runtime logging failure visibility (CSV/archive/mirror/manifest/status/event evidence-sink failures) | `REQ-LOGHEALTH-*` | Implemented (2026-06-20; CSV write failure/recovery events + logger sink detail + `logging_health.json` event-log fallback + status/snapshot retry events + analyzer consistency diagnostics) |
| [FEAT-0023](FEAT-0023-machine-profiles-and-restart-switch.md) | Machine profiles and restart-based profile switch (machine-base/overlay composition + identity resolution + supervisor switch-by-restart, accepting the BIOS-auto gap; optional Rust local helper UI wraps the existing CLI) | `REQ-MPROFILE-*` | Implemented (2026-06-21; composition + active-profile CSV/status + revert integration test done; 2026-06-22 helper UI added; on-hardware live M REQ-MPROFILE-10 PASS 2026-06-25 via Option A2) |
| [FEAT-0024](FEAT-0024-intake-lead-under-load.md) | Intake-lead fan response under load (config-only surge-and-hold: intake-lane joint rise-rate + step-cap raise, intake-first `gpu_airflow` onset, steeper intake `cpu_override` mid-band; idle unchanged, rise-asymmetric) | `REQ-INLEAD-*` | Draft (held — candidate magnitudes pending a response-evaluation Pass-3 validation; idle out of scope) |
| [FEAT-0025](FEAT-0025-amd-gpu-telemetry.md) | AMD GPU telemetry backend (read-only AMD/Radeon GPU temps + logging-only power/context through the existing `GpuReader` seam, so the GPU envelope works on an AMD GPU machine) | `REQ-AMDGPU-*` | Draft (2026-06-22; intake spec landed; D-AMDGPU-1 decision Proposed — ADLX read-only lean; implementation + §12 measurement gate owed; AMD temp ships logging-only until evidence) |
