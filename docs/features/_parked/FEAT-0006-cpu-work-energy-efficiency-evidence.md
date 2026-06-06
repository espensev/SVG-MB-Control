# FEAT-0006: CPU work & energy efficiency evidence

> **Reserved / parked 2026-06-06.** Demoted from Draft to Reserved; this full
> body is preserved under `docs/features/_parked/` pending FEAT-0004 shipping and
> a read-only live MSR feasibility check. The active registry row is in
> `docs/features/README.md` §5. While parked, this body is not part of the
> enforced feature set (`tests/test_feature_specs.py`) and its `REQ-CPUEFF-*`
> rows are not mirrored in `docs/TRACEABILITY.md`.

**Project:** svg-mb-control
**Status:** Reserved (parked; body preserved)   **Version:** 0.1   **Updated:** 2026-06-06
**Namespace:** `REQ-CPUEFF-*`
**Companion to:** `AGENTS.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`, `docs/READ_LOOP.md`,
`docs/MEASUREMENT_GATE.md`, `docs/features/FEAT-0002-cpu-settings-evidence-logger.md`,
`docs/features/FEAT-0004-hardware-access-health-signal.md`
**Purpose:** capture enough first-party raw evidence to evaluate CPU-setting
changes by **work per unit energy** — "same work, less power" — instead of by
time-utilization alone.

> **Need-first spec.** This spec fixes *what must be measured* (the target), then
> names the acquisition work to go get it. The target is not capped by what the
> current PawnIO/SMN read path already exposes. Acquisition (§9, §11) is a
> downstream problem committed to *after* the target is agreed.

## 1. Summary

This feature adds first-party raw CPU **work** and **energy** evidence so an
operator can later compute CPU efficiency — work done per Joule — and compare it
across CPU-setting changes. The north-star derived metric is
**(instructions retired, or delivered cycles) per Joule**, reported with
**average package power** and **effective frequency** as context. This is the
layer above FEAT-0002: FEAT-0002 logs whole-system busy *time*; this feature adds
the *work* numerator and the *energy* denominator that time-utilization cannot
provide.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named contract/capability gap, not an observed runtime-log failure.

FEAT-0002 adds whole-system CPU busy time (`system_cpu_busy_pct` from
`GetSystemTimes`, see `src/control/control_scheduler.cpp`). Busy time measures
*how long* cores were not idle, not *how much work* was done. The moment a CPU
setting changes the operating clock or voltage, the same busy percentage maps to
a different amount of delivered work, so busy time alone cannot answer the
operator's actual question: **did this setting do the same work for less power?**

The current first-party hardware read path reads only **SMN temperature
registers** through PawnIO (`src/hardware/amd_reader.cpp`: Tctl `0x00059800`, CCD
temps `0x00059954` / `0x00059B08`). It does not read the counters that a work or
energy measurement requires (performance-monitoring counters, APERF/MPERF,
RAPL/energy MSRs, SMU power-reporting). FEAT-0002 §3 lists CPU package power,
effective clocks, voltage, and per-core counters as a non-goal "until a
first-party source is designed and reviewed." This feature is that design.

## 3. Goals & non-goals

**Goals**
- Define and capture the minimum raw fields needed to derive **work per Joule**
  per comparable workload window (§5 Tier 1).
- Capture the validity-context fields that keep an efficiency comparison honest —
  effective frequency, voltage, temperature, and the throttle/limit reason
  (§5 Tier 2) — so a lower-power result is distinguishable from a throttled one.
- Keep all logged data raw, so the efficiency ratio and average power are derived
  by analyzer/reporting later, not baked into the logger.
- Carry an operator workload + CPU-setting label so before/after windows group
  without guessing from timestamps.

**Non-goals**
- No fan-control, cadence, channel, or write behavior change. This is read-only
  evidence.
- No third-party sensor tool, subprocess adapter, HWiNFO integration, or
  sibling-repo bridge (`AGENTS.md` §Repo Boundary).
- The controller must not write, set, or program any CPU power/clock/voltage
  control. Reading counters is in scope; **actuating CPU state is not**.
- No analyzer scoring or "good/bad efficiency" verdict in the logger; the ratio
  and its interpretation are analyzer/reporting work.
- Tier 3 resolution (per-core/per-CCD energy and cycles, C-state and
  P-state/boost residency, uncore/SoC power split, energy-delay product) is out
  of scope for the committed need and is listed for later (§11).

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Acquisition uses the in-repo PawnIO transport and OS APIs only; no third-party sensor process. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | This is read-only telemetry. It must not write fan duty, start/stop, or reset breakers. |
| Hardware access is read-only and bounded | `AGENTS.md` §Live Runtime Safety, §Repo Boundary | New reads are **counter reads only** (PMC/MSR/SMN read). The controller must not write CPU control MSRs or SMU control mailboxes. The set of readable registers must be enumerated and reviewed before implementation (§9). |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New CSV/status fields are additive and nullable/blank when unavailable; existing archives remain valid. |
| Shipped cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | Read-only evidence; does not change cadence, channels, or input strategy. Does not cross the gate. |
| Control-computation identity stays unchanged | `docs/CONTROL_PIPELINE_MATH.md` | Records evidence only; no curve, blend, cadence, boost, or write-gate change. |

## 5. The need — target measurement set

The committed need is **Tier 1 + Tier 2**. The north-star derived metric is
work-per-Joule with average power and effective frequency as context. The logger
records raw counters/samples; the ratios below are derived later.

### Tier 1 — irreducible (efficiency is not computable without these)

| Need | Raw signal (proposed) | Derivation it enables |
|---|---|---|
| Work done | instructions retired (PMC `INST_RETIRED`) — ideal; and/or delivered cycles (APERF) | numerator of work-per-Joule |
| Time normalization | reference cycles (MPERF) + window duration + busy time (`system_cpu_busy_pct`, FEAT-0002) | effective frequency; idle vs active fraction |
| Energy cost | package energy counter (RAPL-class, monotonic, differenced over the window) | Joules and average power (ΔEnergy ÷ Δtime) |

### Tier 2 — validity context (without these the efficiency number misleads)

| Need | Why it is required |
|---|---|
| Effective frequency (from APERF/MPERF) | the realized clock — the variable a CPU setting moves |
| Vcore / VID | the primary lever for "same work, less power" |
| Temperature (already logged: Tctl/CCD) | leakage rises with temperature; thermal throttling confounds the comparison |
| Throttle / limit reason — PPT/TDC/EDC-limited, or thermal (HTC/PROCHOT) | distinguishes a genuinely more-efficient window from a power- or thermally-throttled one |
| Workload label + CPU-setting label | groups before/after windows of the same workload class apples-to-apples |

### Derived later (analyzer/reporting, not the logger)

- cycles per Joule (APERF ÷ ΔEnergy) — frequency-aware efficiency.
- instructions per Joule (`INST_RETIRED` ÷ ΔEnergy) — true efficiency when a PMC
  source is available.
- score per Joule — for a fixed repeatable workload with an external score
  (e.g. `tools/eval_cinebench.py`).
- effective IPC (`INST_RETIRED` ÷ APERF) — sanity/quality check on the work proxy.
- average power (ΔEnergy ÷ Δtime); effective frequency (base × ΔAPERF ÷ ΔMPERF).

A window with any unavailable Tier 1 source leaves the dependent fields
blank/null and emits no false zero, per FEAT-0002's logging rule.

## 6. Requirements  *(promotion gate 4 — assign after the acquisition decision picks a direction)*

> These IDs are reserved in this feature's `REQ-CPUEFF-*` namespace. They are not
> normative until the §9 acquisition design decision is written and marked
> current and the §13 gates pass.

| ID | Requirement |
|---|---|
| REQ-CPUEFF-01 | The logger must record a first-party CPU **work** signal per window — instructions retired and/or delivered cycles (APERF) with reference cycles (MPERF) — sufficient to derive a work numerator. |
| REQ-CPUEFF-02 | The logger must record a first-party CPU **package energy** signal per window, as a differenced monotonic counter, sufficient to derive Joules and average power. |
| REQ-CPUEFF-03 | The logger must record Tier 2 validity context: effective frequency, Vcore/VID, and a throttle/limit reason, alongside the already-logged temperature. |
| REQ-CPUEFF-04 | All new fields must be additive and nullable/blank when unavailable; existing CSV archives and runtime-home files must remain valid. A window with an unavailable source must emit no false zero. |
| REQ-CPUEFF-05 | Acquisition must be read-only: counter/register **reads** only. The controller must not write CPU control MSRs, SMU control mailboxes, or any CPU power/clock/voltage actuation. |
| REQ-CPUEFF-06 | The exact set of readable registers/counters must be enumerated and reviewed against `AGENTS.md` §Live Runtime Safety / §Repo Boundary before implementation. |
| REQ-CPUEFF-07 | The logger must not classify or score efficiency; work-per-Joule, average power, and any verdict are derived by analyzer/reporting later. |
| REQ-CPUEFF-08 | The feature must associate windows with an operator workload label and CPU-setting label without making either a measured sensor value. |

## 7. Data / schema deltas

- New runtime CSV/status fields, additive (final names settled at implementation):
  Tier 1 — work counters (instructions retired and/or APERF/MPERF deltas),
  package-energy delta (Joules) and/or raw energy-counter delta; Tier 2 —
  effective frequency, Vcore, throttle/limit reason; plus workload/setting labels.
- Config impact: undecided; a workload/setting label may come from config or a
  runtime marker (shared open question with FEAT-0002 §8).
- Schema/version impact: update `docs/RUNTIME_HOME.md` and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` at implementation. Existing archives
  must parse with the new fields missing.

## 8. CLI / config / operator surface deltas

- A workload label and a CPU-setting label, supplied by the operator (static
  config first; runtime marker later if needed). UI is out of scope
  (`docs/MEASUREMENT_GATE.md`).
- A diagnostic surface that reports which Tier 1 / Tier 2 sources are available on
  the current machine may be useful, mirroring FEAT-0004's hardware-access health
  signal; decided at implementation.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/cpu-work-energy-acquisition-verification-2026-06-04.md`](../cpu-work-energy-acquisition-verification-2026-06-04.md) | Feasibility input: is `read_msr` reachable? **Done (2026-06-04): yes** — the shipped `AMDFamily17.bin` already exposes `ioctl_read_msr`; APERF/MPERF + RAPL package energy are reachable read-only via the existing transport. Surfaces the 32-bit energy wrap, APERF/MPERF affinity, and INST_RETIRED-needs-writes caveats. | Current |
| `docs/cpu-work-energy-acquisition-decision-YYYY-MM-DD.md` | The committed acquisition path per signal. Lead (from verification): v1 = package energy (`0xC001029B` + unit `0xC0010299`) + APERF/MPERF effective frequency via a read-only MSR allow-list, loaded `strict`; defer `INST_RETIRED` (needs PMC programming = MSR writes), per-core aggregation, and SMU-mailbox Vcore/PPT/TDC/EDC. | Needed |
| `docs/cpu-counter-read-safety-review-YYYY-MM-DD.md` | A Live Runtime Safety review confirming the read set is read-only and bounded (only `ioctl_read_msr` on a fixed MSR allow-list; never `ioctl_write_msr`), that no CPU-control write path is introduced, and how an unsupported (`#GP`) or wrapped counter degrades to blank. May be folded into the acquisition decision. | Needed |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-CPUEFF-01 | T, M | unit/smoke test for work-counter delta math; runtime CSV evidence on a supported machine |
| REQ-CPUEFF-02 | T, M | unit/smoke test for energy-counter delta → Joules/avg-power; runtime CSV evidence |
| REQ-CPUEFF-03 | T, R | test for context-field propagation; review vs `docs/RUNTIME_HOME.md` |
| REQ-CPUEFF-04 | T, R | analyzer-ingest tests with old archives missing the new fields; no-false-zero test |
| REQ-CPUEFF-05 | R | code review: read paths only; no CPU-control write |
| REQ-CPUEFF-06 | R | review of the enumerated register/counter read set vs `AGENTS.md` |
| REQ-CPUEFF-07 | R | review logger code/docs: no baked-in efficiency scoring |
| REQ-CPUEFF-08 | T, R | test/config review for label propagation |

Verify legend: **T** automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke /
pytest); **B** build/release gate; **M** manual runtime measurement (read-only,
`AGENTS.md` §Live Runtime Safety); **R** code review vs the cited contract.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Does a PawnIO module exposing `read_msr` exist or is it packageable for the target machines? | acquisition decision | **Resolved (2026-06-04):** yes — the already-shipped `AMDFamily17.bin` exposes `ioctl_read_msr`; no new module needed. See the verification doc. PMC programming for `INST_RETIRED` is the exception (needs MSR writes) and stays deferred. |
| Work proxy: instructions retired (PMC) vs delivered cycles (APERF) for v1? | implementation | APERF cycles first (no PMC programming); add `INST_RETIRED` if the PMC path is clean. |
| Energy source: RAPL MSR vs SMU power-reporting over SMN? | acquisition decision | RAPL MSR if reachable (monotonic energy counter); SMU mailbox is firmware-version-specific and fragile. |
| PPT/TDC/EDC + voltage source | acquisition decision | SMU mailbox over SMN; treat as best-effort context, blank when the firmware layout is unknown. |
| Where the workload/setting label lives | implementation | Static config label first; runtime marker later (shared with FEAT-0002). |
| Tier 3 (per-core/per-CCD energy+cycles, C-state/P-state residency, uncore split, EDP) | a later feature | Out of scope here; revisit after Tier 1+2 ships. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change fan channels, cadence, write timing, or
  control math. Read-only evidence.
- **Depends on:** FEAT-0002 (whole-system busy time is the time-normalization
  context); the PawnIO transport (`src/hardware/amd_reader.cpp`) and its
  availability signal (FEAT-0004).
- **Build/test impact:** counter-delta math tests, analyzer-ingest compatibility
  tests, and the enumerated-read-set review. No `CONTROL_PIPELINE_MATH.md` update
  unless control computation changes, which this feature must not do.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem stated as a named capability/contract gap: busy time is not work;
  the read path reads only SMN temperature today (§2).
- [x] 2. Stressed invariants identified — Repo Boundary, Live Runtime Safety
  (read-only, bounded register reads), runtime-home schema, Measurement Gate,
  control identity (§4).
- [ ] 3. Required design decision record(s) written and marked current — the
  acquisition path and the counter-read safety review (§9).
- [ ] 4. Concrete `REQ-CPUEFF-*` IDs confirmed after the acquisition decision picks
  a direction (§6, reserved).
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [ ] 6. Confirm read-only/bounded hardware access and no Measurement Gate move —
  pending the enumerated read set and its safety review (§9, REQ-CPUEFF-05/06).
- [x] 7. Doctrine check: current behavior is grounded; proposed behavior is
  labeled proposed; Tier 3 named as out of scope.

> Gates 3, 4, and 6 are open: this spec is `Draft` until the acquisition design
> decision and the counter-read safety review exist and are marked current. It is
> not buildable work yet. The committed *target* (Tier 1 + Tier 2, work-per-Joule)
> is agreed; the *acquisition* is the next artifact.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-CPUEFF-01 | | | |
| REQ-CPUEFF-02 | | | |
| REQ-CPUEFF-03 | | | |
| REQ-CPUEFF-04 | | | |
| REQ-CPUEFF-05 | | | |
| REQ-CPUEFF-06 | | | |
| REQ-CPUEFF-07 | | | |
| REQ-CPUEFF-08 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. Update §5/§6 and the cited contract docs if behavior changes, and
bump **Updated**.>
