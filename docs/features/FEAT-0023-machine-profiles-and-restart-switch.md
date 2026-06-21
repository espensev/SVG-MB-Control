# FEAT-0023: Machine profiles and restart-based profile switch

**Project:** svg-mb-control
**Status:** Implemented (composition + active-profile CSV/status fields + revert integration test done; on-hardware live M deferred)   **Version:** 0.4   **Updated:** 2026-06-21
**Namespace:** `REQ-MPROFILE-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`,
`docs/MEASUREMENT_GATE.md`,
`docs/multiprofile-restart-switch-decision-2026-06-20.md`,
`docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md`,
`docs/features/FEAT-0003-selectable-profile-hot-swap.md`
**Purpose:** let one build run across several machines by resolving a named
control profile (machine-base + behavior-overlay) at startup, and let an operator
switch the active profile on a running controller by restarting the worker into a
different profile (a supervised restart, not an in-process swap).

> **Implemented 2026-06-21** (authorized 2026-06-20 ahead of FEAT-0003). All
> seven promotion gates are met and the decision record is Current. FEAT-0023
> shipped first; the control-law/PID seam (FEAT-0003) is sequenced after it and
> was itself promoted to Accepted 2026-06-21. Live runtime switching still
> requires explicit live-runtime authorization when tested on hardware (the
> on-hardware live M, REQ-MPROFILE-10, stays deferred).

## 1. Summary

Today the controller loads exactly one `--config` file; there is no named-profile
concept for the control loop and no machine-identity resolution, and the
per-machine `config/machines/*.cooling.policy.json` is documentation the worker
never loads. This feature introduces a profile catalog where a profile composes a
machine-base (the physical fan inventory/roles already in the policy schema) and a
behavior-overlay (the `control_loop` curves/boosts), resolved into the existing
`ControlConfig`. The controller resolves its profile at startup by explicit
selection or machine identity, falling back to today's default. An operator can
switch the active profile on a running controller; the switch is delivered by the
supervisor restarting the worker into the new profile. During the ~1–2 s restart
gap the fans revert to BIOS SmartFan auto (the existing graceful-restore
behavior), which is accepted rather than latched. The operator-visible outcome is:
each machine auto-selects its own profile, and an operator can switch behavior
profiles on a running box without hand-editing config.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, not an observed runtime failure.

1. **Single baked config, restart-only change.** The supervisor bakes one
   `--config <path>` into the worker command line
   (`src/control/control_supervisor.cpp:553`, from the captured
   `config_source_path` at `:542`/`:732`); each worker re-reads that file at
   startup (`src/app/app_main.cpp:82-93`, `:340-344`). There is no `--profile`
   surface for the control loop (`--profile` exists only on `analyze report`,
   `src/app/app_args.cpp`) and no runtime path to change the active profile.
2. **No machine identity.** No host-name / machine-id resolution exists in the
   worker (`src/platform/service_probe.cpp`, `src/platform/env_util.cpp`,
   `src/platform/runtime_util.cpp` read no host name); the same build cannot
   auto-select per-machine behavior.
3. **Policy JSON is documentation only.** `config/machines/snd-desk.cooling.policy.json`
   (schema `svg_mb_control.machine_cooling_policy.v1`) is consumed only by Python
   tests/analysis (`tests/test_config_contracts.py`,
   `scripts/analyze_cpu_temp_power.py`); the worker never loads it
   (`src/control/control_config.cpp`, `control_loop_config.cpp` read only the
   `--config` file). Per-machine physical facts are not available to the loader.
4. **The supervisor cannot cycle the worker on demand.** `RunSupervisorWorkerLoop`
   (`control_supervisor.cpp:539-670`) respawns only on a non-zero crash exit and
   breaks the loop (exiting the supervisor) on a clean exit; there is no operator
   path to restart the worker into a different config without a full
   teardown + fresh-supervisor `--restart`.

Motivation: the controller is being deployed across several machines/machine
types and an operator wants per-machine profiles and a way to switch behavior on a
running machine without editing config by hand.

## 3. Goals & non-goals

**Goals**

- A profile catalog where a profile composes a machine-base (board/case/fan
  inventory/roles) and a behavior-overlay (`control_loop` curves/boosts) into the
  existing `ControlConfig`/`ControlLoopConfig` shape.
- Startup profile resolution by explicit selection or machine identity, with a
  built-in default fallback; an absent catalog leaves today's `--config` behavior
  unchanged.
- An operator-initiated profile switch on a running controller, delivered by the
  supervisor restarting the worker into the new profile, with validate-before-
  activate, a zero-backoff cycle, and auto-revert to last-known-good.
- An additive active-profile-name + resolution-source field in status and the
  standard control-loop CSV, plus switch applied/rejected/reverted events.

**Non-goals**

- No in-process / mid-tick law or profile swap (that live-swap shape is
  `FEAT-0003`'s, and is explicitly not used here).
- No acoustically seamless switch: the switch accepts the BIOS-auto gap and does
  not add a no-restore duty-latch path or a fan-safety watchdog.
- No control-law / PID seam — that stays `FEAT-0003`, restart-selected and
  sequenced after this feature.
- No change to the shipped 250 ms cadence, write cooldown, channel set, or
  control-computation identity for a machine's default profile.
- No GUI; the operator surface is the runtime-home request file + CLI.
- No sibling-repo, subprocess bridge, or new external sensor dependency.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Catalog, resolver, and supervisor switch are all in-repo; the machine-base reuses the existing in-repo policy schema; no external process is added. |
| No fan write / start / stop / breaker reset / restart outside an explicit live task | `AGENTS.md` §Live Runtime Safety | A switch is delivered only from an explicit operator request consumed by the supervisor, and emits an applied/rejected/reverted event for every outcome. The spec itself is read-only; live switching requires explicit authorization. |
| Shipped 250 ms cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | A machine's default profile must reproduce today's shipped cadence/cooldown/channel set and control identity; a profile that would change cadence/channels/strategy crosses the gate and is out of scope (§12). |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | The resolved default profile composes to the same `ControlLoopConfig` as today's `control.release.json`; profile fields do not enter setpoint computation, boost stages, source selection, or write gates. |
| Runtime sidecar / status / manifest / CSV schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The new request file, the active-profile status/CSV field, and the switch events are additive; an absent catalog/profile and old archives stay valid. |

## 5. Behavior specification

Proposed behavior (not yet implemented).

- **Profile catalog & composition.** A named-profile catalog resolves each
  profile to the existing in-memory `ControlConfig`/`ControlLoopConfig` by
  composing a machine-base (physical identity from the
  `svg_mb_control.machine_cooling_policy.v1` schema, promoted from documentation
  to a loaded input) with a behavior-overlay (the `control_loop` curves/boosts/
  cadence). `config/control.release.json` remains the baked default composition
  and fallback. With no catalog or no resolved profile, the loader behaves exactly
  as today's single-`--config` load (`control_config.cpp:119`,
  `control_loop_config.cpp:507`).
- **Startup resolution & precedence.** Resolution slots into the config-path
  chain at `app_main.cpp:69-94`, ahead of `LoadControlConfig`. Precedence,
  highest first: explicit `--config <file>` → `--profile <name>` /
  `SVG_MB_PROFILE` → machine identity (`GetComputerNameW` plus an optional
  `runtime_home/machine_id.txt` override) → built-in default. Identity resolution
  runs only when no explicit config/profile is given, so `--config` always wins.
  `--show-config` reports the resolved profile name, resolution source, and config
  path.
- **Switch request (supervisor-owned).** A profile switch is delivered as a
  supervisor-consumed, take-once `profile.switch.request.json` at the runtime-home
  root (modeled on the breaker-reset Request/Take/Clear trio,
  `runtime_lifecycle.cpp:40-103`), naming the target profile, written by an
  explicit operator subcommand parallel to `--reset-breakers`
  (`app_main.cpp:164-184`). Absence of the request means no change. The request is
  consumed by the supervisor loop (`control_supervisor.cpp:550`), not the tick
  runner, because the switch is delivered by restarting the worker. An
  active-profile pointer the supervisor re-resolves each loop iteration replaces
  the captured-once `config_source_path` so revert is atomic.
- **Validate before activate.** At the top of the loop the supervisor takes the
  request and load-validates the candidate profile (reuse `LoadControlConfig` /
  `LoadControlLoopConfig`). On parse/validation failure it keeps the running
  worker untouched, emits a rejection event, and clears the request without
  cycling. (Supervisor-side validation is parse-only; a profile that parses but
  fails hardware bind is not caught here — see §11.)
- **Graceful worker cycle (no crash backoff).** On a valid candidate, repoint the
  active-profile pointer and **gracefully stop** the current worker — a
  worker-scoped cooperative stop that runs the worker's shutdown restore so fans
  revert to the captured BIOS baseline, distinct from the global stop request that
  also ends the supervisor — escalating to force-terminate only if the worker does
  not stop within the stop timeout (reuse the FEAT-0008 `EscalateForceTerminate`
  hung-worker path). Then respawn a worker on the new profile, skipping the crash
  backoff (`control_supervisor.cpp:662-667`) and not incrementing the crash
  `restart_count` (`:642`), so the intentional cycle is distinguished from a crash.
  The no-backoff bound is the crash backoff only, not the graceful-stop wait, so a
  switch is not instantaneous.
- **Auto-revert to last-known-good.** If the post-switch worker fails to start,
  the supervisor repoints the pointer to the last profile that produced a worker
  which survived startup and respawns from it, instead of looping on the failing
  profile or exiting; `restart_count` resets once a worker survives startup, and
  the current first-spawn early-return (`control_supervisor.cpp:632-633`) is
  removed so an operator-switch failure self-heals.
- **Switch gap = existing graceful restore.** The switch uses the existing
  graceful-restore shutdown path, so during the worker gap the fans revert to the
  captured BIOS baseline (BIOS SmartFan auto), `control_loop.cpp:236-241` →
  `fan_sio.cpp:925-935`. This feature does not add a no-restore duty-latch path or
  a fan-safety watchdog (decision D-MPROFILE-2; measured gap behavior in
  `docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md`).
- **Observability.** The active profile name and resolution source are recorded
  in runtime status and as an additive standard control-loop CSV field; a switch
  emits applied/rejected/reverted events. These fields and events are
  observational and must not feed setpoint computation, write gates, breaker,
  restore, cadence, or channel policy.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-MPROFILE-01 | The controller must resolve an active control configuration from a named-profile catalog where a profile composes a machine-base (board/case/fan inventory/roles) and a behavior-overlay (`control_loop` curves/boosts/cadence) into the existing `ControlConfig`/`ControlLoopConfig` shape; with no catalog or no resolved profile, the loader behavior must equal today's single-`--config` load. |
| REQ-MPROFILE-02 | When neither an explicit `--config` path nor an explicit profile selector is given, the controller must resolve the active profile from machine identity (host name plus an optional `runtime_home` machine-id override file) and fall back to a built-in default profile when identity is unreadable or not in the catalog; identity resolution must not run when `--config` or a profile selector is given. |
| REQ-MPROFILE-03 | The startup profile-selector precedence must be explicit `--config` over `--profile`/`SVG_MB_PROFILE` over machine identity over the built-in default, and the resolved profile name, resolution source, and config path must be reported by `--show-config`. |
| REQ-MPROFILE-04 | A profile switch on a running controller must be delivered as a supervisor-consumed, take-once request (modeled on the breaker-reset request primitive) naming a target profile, written by an explicit operator subcommand; absence of the request must mean no change, and the request must be consumed by the supervisor loop, not the tick runner. |
| REQ-MPROFILE-05 | On a switch request, the supervisor must load-validate the candidate profile before activating it; on parse or validation failure it must keep the running worker unchanged, emit a rejection event, and clear the request without cycling the worker. |
| REQ-MPROFILE-06 | A validated switch must repoint the active profile, gracefully stop the current worker so its shutdown restore runs (fans revert to the captured BIOS baseline), escalating to force-terminate only if the worker does not stop within the stop timeout (the FEAT-0008 hung-worker path), and then respawn a worker on the new profile without applying the crash-restart backoff and without incrementing the crash restart counter; the no-backoff bound applies to the crash backoff only, not the graceful-stop wait. |
| REQ-MPROFILE-07 | If a post-switch worker fails to start, the supervisor must revert the active profile to the last profile that produced a worker which survived startup and respawn from it, rather than looping on the failing profile or exiting the supervisor; the crash restart counter must reset once a worker survives startup. |
| REQ-MPROFILE-08 | The switch must stop the worker through the existing graceful-restore shutdown path so fans revert to the captured BIOS baseline during the worker gap (force-terminate is only the hung-worker fallback, not the normal switch path); this feature must not add a no-restore duty-latch path or a separate fan-safety watchdog. |
| REQ-MPROFILE-09 | The active profile name and resolution source must be recorded in runtime status and as an additive standard control-loop CSV field, and a switch must emit applied, rejected, and reverted events; these fields and events must be observational and must not feed setpoint computation, write gates, breaker, restore, cadence, or channel policy. |
| REQ-MPROFILE-10 | Profile composition and selection must not change the shipped 250 ms cadence, write cooldown, channel set, or control-computation identity for a machine's default profile; the resolved default profile must reproduce today's `config/control.release.json` behavior, and a profile that would change cadence/channels/strategy crosses `docs/MEASUREMENT_GATE.md` and is out of this feature's scope. |

## 7. Data / schema deltas

- **New config / catalog artifacts:** a profile catalog (machine-base entries from
  the existing `svg_mb_control.machine_cooling_policy.v1` schema + behavior-overlay
  entries) plus an active-profile pointer file at the runtime-home root. Exact
  on-disk layout (single catalog file vs. per-profile files; pointer name) is
  settled at implementation; `config/control.release.json` stays a valid baked
  default with no edits.
- **New runtime-home request file:** `profile.switch.request.json` at the
  runtime-home root, take-once, naming the target profile (modeled on
  `circuit_breaker_reset.request.json`). Absent = no change.
- **New status + CSV fields:** active profile name and resolution source in
  runtime status and an additive standard control-loop CSV field
  (`src/runtime/runtime_csv_rows.cpp`, control-loop header/row only). Additive and
  nullable; old archives stay valid; analyzer binds by header name.
- **New event types:** `supervisor.profile_applied` / `supervisor.profile_rejected`
  / `supervisor.profile_reverted` (names settled at implementation).
- **Schema/version impact:** additive only. The analyzer ingest schema
  (`src/analyze/analyze_db.h` `kSchemaVersion`) needs a `+1` migration if the
  active-profile CSV field is ingested; update `docs/RUNTIME_HOME.md`,
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md`, and the relevant mode docs at
  implementation. No existing config or runtime-home file becomes invalid.

## 8. CLI / config / operator surface deltas

- **New `--profile <name>` startup selector** and `SVG_MB_PROFILE` env, resolved
  in the `app_main.cpp` config-path chain below an explicit `--config`.
- **New operator subcommand** to write the profile-switch request (a subcommand
  parallel to `--reset-breakers`), respecting `AGENTS.md` §Live Runtime Safety as
  an explicit opt-in action.
- **`--show-config`** reports the resolved profile name, resolution source, and
  config path.
- Update `README.md` (operator workflow + profile selection), `docs/CONTROL_LOOP.md`,
  `docs/WRITE_ORCHESTRATION.md` (supervisor switch path), and `docs/RUNTIME_HOME.md`
  (request file, status field, events) per `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/multiprofile-restart-switch-decision-2026-06-20.md`](../multiprofile-restart-switch-decision-2026-06-20.md) | Restart-based switching vs in-process swap (D-MPROFILE-1); accept the BIOS-auto gap with no latch/no fan watchdog (D-MPROFILE-2); machine-base + behavior-overlay model (D-MPROFILE-3); identity resolution + precedence (D-MPROFILE-4); supervisor-owned switch with validate-before-activate, zero-backoff cycle, and auto-revert (D-MPROFILE-5); the control-law/PID seam stays FEAT-0003, restart-selected and sequenced after (D-MPROFILE-6). | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-MPROFILE-01 | T, R | `.\scripts\Test-LocalCI.ps1` — composition test: a resolved default profile produces a `ControlLoopConfig` equal to today's `control.release.json`; no-catalog path equals current load; review vs the decision record. |
| REQ-MPROFILE-02 | T, R | resolution tests: identity resolves a profile; unreadable/unknown identity falls back to default; identity does not run with `--config`/profile given. |
| REQ-MPROFILE-03 | T, R | precedence tests across `--config` / `--profile` / `SVG_MB_PROFILE` / identity / default; `--show-config` reports name, source, and path. |
| REQ-MPROFILE-04 | T, R | test: the supervisor consumes a take-once `profile.switch.request.json`; absence means no change; review the take-once shape vs `runtime_lifecycle.cpp` breaker-reset. |
| REQ-MPROFILE-05 | T, R | test: an invalid candidate is rejected, the running worker is untouched, a rejection event is emitted, and the request is cleared. |
| REQ-MPROFILE-06 | T, R | test: a valid switch gracefully stops the worker (restore runs), escalates to force-terminate only on stop timeout, and respawns without crash backoff and without incrementing `restart_count`; review the cycle path vs `control_supervisor.cpp` and the FEAT-0008 stop/escalate pattern. |
| REQ-MPROFILE-07 | T, R | test: a post-switch startup failure reverts to last-known-good and respawns; `restart_count` resets on a surviving worker; supervisor does not exit on an operator-switch failure. |
| REQ-MPROFILE-08 | T, R | test/review: the switch path calls the existing graceful restore and adds no no-restore latch path or fan-safety watchdog; review vs `control_loop.cpp` restore + decision D-MPROFILE-2. |
| REQ-MPROFILE-09 | T, R | CSV/status field + event tests; review confirms the active-profile field/events are not read by setpoint, boost, write, breaker, restore, cadence, or channel policy. |
| REQ-MPROFILE-10 | T, R, M | composition test that the default profile reproduces shipped `ControlLoopConfig`; review vs `docs/MEASUREMENT_GATE.md` that cadence/cooldown/channel set/identity are unchanged; runtime evidence on a deployed default profile. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Catalog on-disk layout (single catalog file vs per-profile files) and pointer file name | implementation | Per-machine base + shareable overlay; pointer at runtime-home root. |
| Tightening the validate/revert window for a parse-valid / bind-invalid or slow-hanging profile | implementation | Out of the first slice; the BIOS-auto gap keeps the failure mode safe (fans on BIOS auto), and auto-revert covers fast-exit failures (§5; decision D-MPROFILE-5 known limit). |
| Machine identity source if host name is unsuitable on a rig | implementation | `GetComputerNameW` + optional `runtime_home/machine_id.txt` override. |
| Whether the active-profile CSV field is ingested by the analyzer (schema `+1`) or status-only | implementation | Status + CSV field; analyzer ingest decided with the schema bump. |

## 12. Measurement gate & dependencies

- **Measurement gate:** a machine's default profile must reproduce the shipped
  250 ms cadence, write cooldown, channel set, and control-computation identity, so
  selecting it does not move the `docs/MEASUREMENT_GATE.md` baseline
  (REQ-MPROFILE-10). A profile that changes cadence, channels, or controller
  strategy crosses the gate and is out of this feature's scope; the control-law
  (PID) path is `FEAT-0003`, which carries the gate's shadow/dry-run +
  characterization-evidence + slew-cap posture.
- **Depends on:** the supervised-restart machinery
  (`control_supervisor.cpp`), the FEAT-0008 `EscalateForceTerminate` path, the
  breaker-reset request primitive (`runtime_lifecycle.cpp`), and the existing
  machine policy schema (`config/machines/*.cooling.policy.json`). Sequenced before
  `FEAT-0003` (control-law seam).
- **Build/test impact:** new profile-resolution + supervisor-switch modules and
  tests under `tests/`; an additive CSV field + optional analyzer schema `+1`
  migration/degrade test; doc updates per `AGENTS.md` §Change Checklist. No
  `docs/CONTROL_PIPELINE_MATH.md` change (control identity is preserved).

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from a named code/contract gap with file:line evidence (§2).
- [x] 2. Stressed invariants identified — Repo Boundary, Live Runtime Safety, Measurement Gate, control identity, runtime schema stability (§4).
- [x] 3. Required design decision record written and marked Current (§9; `docs/multiprofile-restart-switch-decision-2026-06-20.md`).
- [x] 4. Concrete `REQ-MPROFILE-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary and does not silently move the Measurement Gate baseline (the default profile reproduces the shipped baseline; live switching needs explicit authorization) (§4, §12).
- [x] 7. Doctrine check: claims grounded with file:line; proposed behavior labeled proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined or unqualified vague terms.

> All seven promotion gates are met. **Implemented 2026-06-21** (commits
> `0952e3d` / `1195d84` / `9a78a11` startup resolution + live switch; `79145e4`
> active-profile CSV/status fields, REQ-09; `e431dfd` revert integration test,
> REQ-07; `fb70be5` machine-base/overlay composition, REQ-01). REQ-01/04/05/06/07/
> 08/09 pass; REQ-02/03 pass; REQ-10 is T+R-pass with only the on-hardware live M
> deferred (which also gates wiring the host identity + catalog into Build-Release).
> FEAT-0003's control-law/PID seam remains a later phase.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-MPROFILE-01 | pass | T,R (`fb70be5`). `ComposeConfigRoot` (`profile_composition.{h,cpp}`) composes a `machine_cooling_policy.v1` machine base (per-channel `release_min_duty_pct`) with a behavior overlay (control-config-shaped, per-channel `min_duty_pct` removed), injecting the floor from the base; routed through both `LoadControlConfig` and `LoadControlLoopConfig`; a no-`compose` file is unchanged (no-catalog path = today). Ships `config/overlays/release.behavior.json` + `config/profiles/snd-desk-composed.json`. `profile_composition_tests` proves compose(snd-desk) reproduces `control.release.json` and rejects an uncontrolled overlay channel. | 2026-06-21 |
| REQ-MPROFILE-02 | pass | T,R (`0952e3d`). `machine_identity.{h,cpp}` host-name + `machine_id.txt` override; precedence resolver falls back to the built-in default; `machine_identity_tests` (5), `machine_profile_tests`, `test_machine_profile.py` (env/machine paths). | 2026-06-21 |
| REQ-MPROFILE-03 | pass | T,R (`0952e3d`). Precedence `--config` > `--profile` > `SVG_MB_PROFILE` > machine-id > default; `--show-config` reports name/source; `machine_profile_tests` + `test_machine_profile.py`. | 2026-06-21 |
| REQ-MPROFILE-04 | pass | T,R (`1195d84`/`9a78a11`). Take-once `profile.switch.request.json` consumed by the supervisor (not the tick runner); `runtime_lifecycle_tests` + `test_profile_switch.py`. | 2026-06-21 |
| REQ-MPROFILE-05 | pass | T,R (`9a78a11`). `DecideSwitchRequest` rejects malformed/empty/unknown/invalid candidates; the supervisor validates (`LoadControlConfig`+`LoadControlLoopConfig`) before signaling; `profile_switch_decision_tests` + `test_profile_switch.py` rejection (worker untouched, request cleared, `profile_rejected` event). | 2026-06-21 |
| REQ-MPROFILE-06 | pass | T,R (`9a78a11`). Supervisor cycles without crash backoff; `test_profile_switch.py` happy-path asserts a new worker PID, `profile_applied` event, no `worker_restart_scheduled`, unchanged `worker_restart_count`. | 2026-06-21 |
| REQ-MPROFILE-07 | pass | T,R (`9a78a11`/`e431dfd`). `DecideAfterStartupOutcome` last-known-good promotion + revert is unit-tested and wired in the supervisor (revert before the supervisor-killing guard; `!have_seen_good_worker`). A double-gated sim hook (`SVG_MB_CONTROL_SIM_FAIL_STARTUP_CONFIG_STEM` + matching `--config` stem) now forces a switched-in candidate to fail its own startup; `test_profile_switch.py::test_failed_switch_reverts_to_last_known_good` asserts a `supervisor.profile_reverted` event, that the active profile returns to the baseline, and that the supervisor self-heals (respawns a worker). On-hardware bind-failure M deferred. | 2026-06-21 |
| REQ-MPROFILE-08 | pass | T,R (`9a78a11`). The worker (`control_loop` + `read_loop`) breaks on the cycle signal so the existing graceful restore runs (fans → captured BIOS baseline); no no-restore latch path or fan watchdog added; `test_profile_switch.py` cycles the worker; review vs decision D-MPROFILE-2. | 2026-06-21 |
| REQ-MPROFILE-09 | pass | T,R (`9a78a11`/`79145e4`). Switch events emitted; active profile name in `control_supervisor.json` (supervisor state) AND name + resolution source in the worker runtime status (`control_runtime.json`, both schemas) and as additive control-loop-only CSV columns `active_profile_name`/`active_profile_source` (threaded from `ControlConfig`, never read by control). Under supervision the worker is launched with `--config`, so the name falls back to the config stem (= the switched profile name). `csv_rows_tests` locks the columns; `test_profile_switch.py` asserts the worker status records name + source on a live switch. | 2026-06-21 |
| REQ-MPROFILE-10 | partial | T,R (`fb70be5`). T+R satisfied: `profile_composition_tests` proves the composed snd-desk profile reproduces `control.release.json` field-by-field — full `ControlLoopConfig` (cadence, cooldown, channel set, curves, boosts, low-band) AND the resolved top-level `ControlConfig` incl. runtime paths — so a composed default is a byte-identical drop-in; the no-`compose` default path is untouched. Live **M** on a deployed default profile remains the only open item (and gates wiring the host identity + catalog into Build-Release). | 2026-06-21 |

**Spec vs. implementation deltas:** The catalog supports both whole-config
profiles (`config/profiles/<name>.json`) and the machine-base + behavior-overlay
**composition** of §5 (a descriptor with a `compose` block; `ComposeConfigRoot`).
A composed config resolves its runtime/snapshot/policy paths relative to the
**behavior overlay** (the layer that declares them), so the shipped
`release.behavior.json` uses `..\` prefixes to land in `config/` exactly like
`control.release.json`. The example descriptor is named `snd-desk-composed`
(not `snd-desk`) so it does **not** auto-resolve by machine identity yet —
verified safe by the reproduction test (a byte-identical drop-in) but left as a
live-M-gated deploy step; `Build-Release` deploys only `control.release.json`
today. The live switch added a worker-scoped `profile.cycle.request.json` presence
signal (distinct from the global stop) consumed by both `control_loop` and
`read_loop`, plus a supervisor bounded-wait + force-terminate fallback for a
wedged worker (advisor review) and exit-code discrimination so a coincident crash
is not mistaken for a cycle. The auto-revert path is exercised by a double-gated
startup-fault sim hook (`SVG_MB_CONTROL_SIM_FAIL_STARTUP_CONFIG_STEM`). The only
remaining item is the on-hardware live **M** (REQ-MPROFILE-10).
