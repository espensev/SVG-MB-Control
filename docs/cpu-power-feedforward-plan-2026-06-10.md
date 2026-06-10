# CPU Package-Power Anticipation ("Power Feed-Forward") — Plan Scaffold — 2026-06-10

Status: **proposal / planning scaffold only.** This is not authorized work
(`AGENTS.md` §Feature Intake Gate): no code, config, or behavior change is
permitted from this document. The owning v1 telemetry decision,
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` §Scope, states the
logged energy data "must not feed any control path: no curve, blend, boost,
cadence, write-gate, or feedforward input. A CPU power feedforward is a
separate future feature and is explicitly out of scope here." This document
scaffolds that separate future feature up to (not through) its FEAT intake.

## 1. Intent

CPU package power is the heat input the fans must remove, and it moves at load
onset before `cpu_tctl_c` does (the thermal mass between die power and Tctl is
the lead the controller could exploit; the actual lead time on this machine is
**unmeasured** — measuring it is Gate 2 below). A power-keyed anticipation term
would start radiator/exhaust response during that gap and reduce transient
Tctl overshoot on load steps.

What it cannot do, stated up front:

- It cannot improve steady state: the temperature curve law already converges
  there, and the anticipation term decays once temperature-keyed terms carry
  the demand.
- It cannot create headroom when fans are saturated (the 2026-06-07 y-cruncher
  session reached Tctl 95.5 C with fans saturated; an earlier ramp does not
  change the saturated ceiling).

## 2. Terminology

This repo already uses "feedforward" for `feedforward_pct` — the raw curve
demand $r^{(c)}_k$ in `docs/CONTROL_PIPELINE_MATH.md` §8.4. To avoid colliding
with that identity, this document calls the proposed term the
**power-anticipation boost** $B^{(c,\mathrm{pwr})}$. (It is feed-forward in the
control-theory sense — input-driven, not error-driven — exactly like the
existing curve law.)

## 3. Current state of the energy implementation (the input signal)

As of 2026-06-10 (`analyze-native-superset`, live worker exe `c4b6986`):

- **Producer (landed, default-off):** read-only RAPL package-energy windows
  via the shipped PawnIO `AMDFamily17.bin` `ioctl_read_msr`, two-MSR allow-list
  (`0xC0010299` unit / `0xC001029B` accumulator, `src/hardware/rapl_energy.h`),
  read outside the PCI mutex at the ~1 s resource-window cadence, gated by
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE` (default `disabled`). Validated on this
  Family 1Ah part: ESU = 16 → 15.26 µJ/unit; tracks load; 32-bit wrap ≈ 5–6 min
  at ~200 W.
- **Plumbing (landed):** the window already reaches the control loop's
  snapshot — `runtime_snapshot.h` carries `pkg_energy_sample_id`,
  `pkg_energy_window_ms`, `pkg_energy_delta_uj`, `pkg_energy_acquisition` —
  and is logged as 4 additive CSV columns. The companion per-core APERF/MPERF
  cycle path (`SVG_MB_CONTROL_CPU_CYCLES_MODE`, 5 CSV columns) is landed and
  default-off; its analyzer effective-frequency increment (schema v9→v10) is
  not yet built.
- **Consumer (landed):** analyzer schema v9 derives time-weighted average
  package watts over distinct `cpu_power_sample_id` windows.
- **Trust state: quarantine, zero evidence sessions.** Markers are
  `disabled` live; the enabled path has never run in CI or on hardware.
  Quarantine exit requires all 6 criteria of the decision doc §Evaluation
  across ≥ 3 sessions spanning ≥ 7 days
  (`docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md` is the
  prepared, unexecuted capture procedure).

The plumbing means a control-path consumer is a governance and validation
problem, not a data-path problem.

## 4. Design sketch (proposed, to be settled in the FEAT decision record)

The smallest-surface slot is a **fifth boost stage** in `kBoostStageSpecs`
(`src/control/boost_stage.cpp`): the unified integrator of
`CONTROL_PIPELINE_MATH.md` §6.1 is already input-agnostic (a scalar against a
band $[a,b]$), so the stage would be:

- **Input:** derived package watts
  $W_k = \mathrm{pkg\_energy\_delta\_uj} / \mathrm{pkg\_energy\_window\_ms} / 1000$,
  refreshed when `pkg_energy_sample_id` increments (~1 s), held between
  windows. Optionally smoothed over ≥ 2 windows (EMA) against window noise —
  a Gate 2 decision.
- **Band:** `power_anticipation_start_w` / `_full_w` in watts (numbers must
  come from Gate 2 captures, not be invented; `start_w` sits above the
  measured idle-floor excursions so the stage never fires at idle).
- **Output:** same rise/fall %/s rates and max clamp as the other stages,
  added in §8.2 alongside $B^{\mathrm{thp}}, B^{\mathrm{mid}}, B^{\mathrm{gpu}},
  B^{\mathrm{soak}}, B^{\mathrm{lb,eff}}$; rides the existing rate limiter and
  output gates; lands in `correction_pct`, leaving the `feedforward_pct`
  identity untouched.
- **Fail-safe semantics (differs from existing stages):** an undefined/blank
  input (energy disabled, MSR fault, wrap-guard blank, first window after a
  watchdog restart) decays the boost toward zero rather than holding it —
  on any fault the temperature-keyed law rules alone. This needs a new release
  mode beside `BelowStart`/`ExplicitRelease`.
- **Config:** per-channel, absent parameters = stage disabled (the existing
  §6.1 guard-clause semantics give this for free). Candidate channels are the
  CPU-heat movers (radiator exhausts `1`/`5`, rear exhaust `0`, radiator
  intake `4`); selection is a spec decision from Gate 2 data.
- **Interaction cap:** $B^{\mathrm{pwr}}$ is additive with `thermal_pressure`
  once Tctl catches up; the spec must size the caps so the intended joint
  ceiling holds (or define a composition rule). Unresolved here.

## 5. Gates, in order (each blocks the next)

- **Gate 0 — quarantine exit (already planned, no new authorization):** run
  the capture runbook ≥ 3 sessions over ≥ 7 days; all 6 criteria pass;
  maintainer flips `cpu_pkg_energy_acquisition` to `validated`.
- **Gate 1 — always-on decision:** a control input cannot depend on a
  default-off env var. Requires criterion-6 (no-disturbance) evidence and an
  explicit maintainer decision to make the energy read default-on (a decision
  the v1 doc reserves as a quarantine outcome), or a spec-reviewed rule that
  the read auto-enables iff the stage is configured.
- **Gate 2 — characterization (read-only, no spec required):** from
  enabled-session CSVs, measure (a) power→Tctl lead time on load steps
  (cross-correlation), (b) steady-state watts↔Tctl↔duty relation, (c) idle
  floor and window noise, (d) blank/wrap frequency. Also run the same
  analysis against `system_cpu_busy_pct` (see §6).
  **Go/no-go: if the measured lead time does not exceed the fan-side response
  latency (rate limiter + spin-up) by a useful margin, stop here — the
  feature has no benefit to deliver.**
- **Gate 3 — FEAT intake:** new feature spec (next free FEAT number;
  FEAT-0006 cannot be expanded — its decision record excludes control use),
  with REQ IDs, acceptance criteria, a decision record fixing the band/cap
  numbers from Gate 2 evidence, and `docs/TRACEABILITY.md` entries.
  Implementation starts only when that spec is implementation-authorized.

## 6. Evaluation (judgment, labeled as such)

- **Benefit is bounded to transients.** Expected value concentrates in load
  steps from idle/low: the stage can start exhaust response seconds before
  Tctl crosses `thermal_pressure_start_c`. Steady state and saturated-fan
  events are unaffected (§1).
- **A cheaper alternative exists and must be compared.** FEAT-0002's
  `system_cpu_busy_pct` is already validated, always-on, and also leads Tctl.
  Power is the better physical signal (actual watts including boost
  behavior, not just busy time), but if Gate 2 shows busy-keyed anticipation
  captures most of the lead, it skips Gates 0–1 entirely and the power
  variant should not be built for control (it remains evidence telemetry).
- **Implementation cost is small; validation cost dominates.** The boost-stage
  seam makes the code increment comparable to past stage work, but the
  evidence chain (Gates 0–2, then shadow + multi-session live evaluation) is
  the bulk of the effort — consistent with how FEAT-0006 itself was run.
- **Main technical risks:** duty pumping from window noise near `start_w`
  (mitigations: smootherstep band, low rise rate, EMA, idle-floor-clearing
  start); restart/fault blanks (decay semantics, §4); double-counting with
  `thermal_pressure` (cap composition, §4); and the ~1 s input cadence
  quantizing the anticipation onset (acceptable iff Gate 2's lead time is
  several seconds).

## 7. Phased implementation (after Gate 3 authorization; recorded for scoping)

- **Phase A — shadow mode:** compute $B^{(c,\mathrm{pwr})}$ per tick and log
  it as an additive CSV column (e.g. `channelN_power_anticipation_shadow_pct`)
  WITHOUT adding it into §8.2. Default-off gate, mirroring the energy path's
  own rollout. Exit evidence: on real load steps the shadow term rises a
  measured lead ahead of `thermal_pressure`; zero firings at idle across
  sessions; bounded magnitude.
- **Phase B — replay validation:** unit-test the stage math as a pure
  function (pattern: `rapl_energy.h` / `cpu_cycles.h`), and replay captured
  CSVs through it to compare shadow output against observed Tctl outcomes;
  fix quantitative exit criteria (e.g. predicted overshoot reduction in C on
  the captured steps) in the spec.
- **Phase C — live additive enable:** per-channel rollout with a low initial
  cap, quarantine-style multi-session evaluation (reduced Tctl peak overshoot
  on load steps, no idle duty increase, no oscillation, no regression of the
  temperature-keyed response), maintainer-recorded promotion before any cap
  increase.

## 8. What this document is not

Not a FEAT spec, not a decision record, not implementation permission, and
not a commitment to build: Gate 2 contains an explicit no-go outcome, and the
busy-pct comparison in §6 may end the power variant before intake.
