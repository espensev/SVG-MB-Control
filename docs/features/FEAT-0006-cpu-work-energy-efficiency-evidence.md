# FEAT-0006: CPU work & energy efficiency evidence

> **Promoted to Accepted 2026-06-07** (it reached Draft from Reserved the same
> day). All seven promotion gates (§13) are met, so per
> `docs/features/README.md` §2 the spec is Accepted: agreed and authorizable. The
> v1 acquisition path and the folded counter-read safety review are settled by the
> accepted
> [`cpu-work-energy-acquisition-decision-2026-06-07.md`](../cpu-work-energy-acquisition-decision-2026-06-07.md);
> `REQ-CPUEFF-*` are mirrored in `docs/TRACEABILITY.md` and this body is in the
> enforced feature set (`tests/test_feature_specs.py`). The feature remains
> Accepted rather than Implemented/Done, but the v1 read-only telemetry slices
> have been authorized and landed behind default-off gates: the
> RAPL package-energy logger (2026-06-07), analyzer time-weighted average-power
> derivation (2026-06-09), APERF/MPERF cycle logger (2026-06-09), and analyzer
> effective-frequency derivation (schema v10, 2026-06-10). Status stays
> **Accepted** (not `Implemented`): the enabled integration path now has 3
> independent live sessions passing the energy quarantine-exit criteria (§14),
> but the marker promotion remains a manual maintainer decision, the
> energy↔cycle join for cycles-per-Joule is not specified, cycle criterion 4
> remains manual, and REQ-CPUEFF-08 remains deferred. Logged energy/cycles ship
> `quarantine` until the post-implementation Evaluation promotes them (§13,
> §14).

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.6   **Updated:** 2026-06-14
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

> **v1 scope (energy + delivered-cycle evidence; corrected 2026-06-09).** As
> currently implemented, v1 acquires package energy / average power and, when the
> default-off cycle path is enabled, APERF/MPERF delivered/reference cycles for
> analyzer-derived effective frequency. The 2026-06-07 live validation reported
> the work counters as `#GP`, but that was a **probe-index error**: the shipped
> PawnIO `AMDFamily17.bin` allow-lists `ioctl_read_msr` for the AMD read-only
> aliases `MSR_MPERF_RO 0xC00000E7` / `MSR_APERF_RO 0xC00000E8` (not the
> architectural `0xE7`/`0xE8` the probe read). The logger now emits those raw
> deltas when explicitly enabled; the analyzer derives ΔAPERF/ΔMPERF and
> effective MHz when supplied a P0 base. `INST_RETIRED` still needs PMC writes
> and stays out of read-only scope. The energy↔cycle join for cycles-per-Joule is
> not specified yet because the two paths carry independent sample ids. Evidence:
> [`docs/cpu-work-energy-live-validation-results-2026-06-07.md`](../cpu-work-energy-live-validation-results-2026-06-07.md).

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
- Capture the v1 validity context that the accepted read-only MSR path can
  support: APERF/MPERF-derived effective-frequency inputs when live validation
  passes, already-logged CPU temperature, per-signal acquisition/provenance, and
  workload/setting labels. Vcore/VID and PPT/TDC/EDC throttle context remain
  Tier 2 target signals, but are deferred from v1 because the accepted
  acquisition decision keeps SMU-mailbox work out of this slice.
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
- No Vcore/VID or PPT/TDC/EDC throttle/limit acquisition in v1. Those require a
  separate SMU/SVI source decision before they can become measured fields.
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

The committed need is **Tier 1 + the v1 Tier 2 subset**. The north-star derived
metric is work-per-Joule with average power and effective frequency as context.
The logger records raw counters/samples; the ratios below are derived later.
The broader Tier 2 context remains the target model, but only fields with a
reviewed first-party source are part of v1.

### Tier 1 — irreducible (efficiency is not computable without these)

| Need | Raw signal (proposed) | Derivation it enables |
|---|---|---|
| Work done | instructions retired (PMC `INST_RETIRED`) — ideal; and/or delivered cycles (APERF) | numerator of work-per-Joule. **v1 emits delivered cycles when explicitly enabled** — delivered cycles (ΔAPERF) read via the shipped bin's RO alias `0xC00000E8` (the 2026-06-07 `#GP` was a probe-index error; corrected 2026-06-09, §11); `INST_RETIRED` needs PMC writes and stays deferred. |
| Time normalization | reference cycles (MPERF) + window duration + busy time (`system_cpu_busy_pct`, FEAT-0002) | effective frequency; idle vs active fraction. **v1 emits MPERF when explicitly enabled** via the shipped bin's RO alias `0xC00000E7`; window duration + FEAT-0002 busy time are today's time-normalization context. |
| Energy cost | package energy counter (RAPL-class, monotonic, differenced over the window) | Joules and average power (ΔEnergy ÷ Δtime) |

### Tier 2 — validity context (without these the efficiency number misleads)

| Need | Why it is required | v1 disposition |
|---|---|---|
| Effective frequency (from APERF/MPERF) | the realized clock — the variable a CPU setting moves | Include inputs only when live validation confirms APERF/MPERF affinity correctness; otherwise leave unavailable. |
| Vcore / VID | the primary lever for "same work, less power" | Deferred from v1; needs a separate SMU/SVI source decision. |
| Temperature (already logged: Tctl/CCD) | leakage rises with temperature; thermal throttling confounds the comparison | Already logged; keep aligned with the work/energy window. |
| Throttle / limit reason — PPT/TDC/EDC-limited, or thermal (HTC/PROCHOT) | distinguishes a genuinely more-efficient window from a power- or thermally-throttled one | Deferred from v1; needs a separate SMU/firmware source decision. |
| Workload label + CPU-setting label | groups before/after windows of the same workload class apples-to-apples | Include as operator labels, not measured sensor values. |

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

## 6. Requirements  *(promotion gate 4)*

> These IDs are active planning requirements for this enforced, Accepted spec.
> They are mirrored in `docs/TRACEABILITY.md`. The v1 read-only telemetry slices
> are implemented and tested
> (REQ-CPUEFF-01/02/04/05/06/07, and REQ-CPUEFF-03 in its v1 scope — see §14);
> REQ-CPUEFF-01 is implemented as delivered/reference cycles (APERF/MPERF) behind
> a default-off gate and still needs the multi-session quarantine judgment; the
> energy↔cycle join for cycles-per-Joule is not specified yet; REQ-CPUEFF-08
> (operator label) is unimplemented in v1.

| ID | Requirement |
|---|---|
| REQ-CPUEFF-01 | The logger must record a first-party CPU **work** signal per window — instructions retired and/or delivered cycles (APERF) with reference cycles (MPERF) — sufficient to derive a work numerator. **Implemented as a default-off APERF/MPERF path (corrected 2026-06-09):** the shipped `AMDFamily17.bin` allow-lists the AMD read-only aliases `0xC00000E7`/`0xC00000E8`, so delivered cycles (ΔAPERF) and reference cycles (ΔMPERF) are readable with the bin already shipped — the 2026-06-07 `#GP` was a probe-index error (it read the architectural `0xE7`/`0xE8`). `INST_RETIRED` still needs PMC writes (out of read-only scope), but APERF delivered-cycles is the v1 work proxy. Energy (REQ-CPUEFF-02) remains the denominator; the cycles-per-Joule join is pending because energy and cycle windows carry independent sample ids. |
| REQ-CPUEFF-02 | The logger must record a first-party CPU **package energy** signal per window, as a differenced monotonic counter, sufficient to derive Joules and average power. |
| REQ-CPUEFF-03 | The v1 logger must preserve available validity context for efficiency analysis: existing CPU temperature remains available with the work/energy window; effective-frequency inputs from APERF/MPERF are emitted only when the default-off cycle path is enabled and are derived by the analyzer rather than guessed; Vcore/VID and throttle/limit reasons remain unavailable/deferred rather than guessed or emitted as measured values. |
| REQ-CPUEFF-04 | All new fields must be additive and nullable/blank when unavailable; existing CSV archives and runtime-home files must remain valid. A window with an unavailable source must emit no false zero. |
| REQ-CPUEFF-05 | Acquisition must be read-only: counter/register **reads** only. The controller must not write CPU control MSRs, SMU control mailboxes, or any CPU power/clock/voltage actuation. |
| REQ-CPUEFF-06 | The exact set of readable registers/counters must be enumerated and reviewed against `AGENTS.md` §Live Runtime Safety / §Repo Boundary before implementation. |
| REQ-CPUEFF-07 | The logger must not classify or score efficiency; work-per-Joule, average power, and any verdict are derived by analyzer/reporting later. |
| REQ-CPUEFF-08 | The feature must associate windows with an operator workload label and CPU-setting label without making either a measured sensor value. |

## 7. Data / schema deltas

- New runtime CSV/status fields, additive (final names settled at implementation):
  Tier 1 — a resource-window sample id and duration, work counters
  (instructions retired and/or APERF/MPERF deltas), package-energy delta
  (Joules) and/or raw energy-counter delta, and per-signal acquisition markers
  so package energy and cycles can be `disabled`, `unavailable`, `quarantine`,
  or `validated` independently; v1 context — the APERF/MPERF inputs needed for
  analyzer-derived effective frequency when validated, already-logged
  temperature aligned with the window, optional blank-reason fields, and
  workload/setting labels. Vcore and throttle/limit reason fields are deferred
  until a separate source decision exists. The sample id/window are required so
  an analyzer can avoid double-counting one resource-window delta repeated
  across multiple 250 ms control-loop rows.
- Config impact: undecided; a workload/setting label may come from config or a
  runtime marker (shared open question with FEAT-0002 §8).
- Schema/version impact: update `docs/RUNTIME_HOME.md` and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` at implementation. Existing archives
  must parse with the new fields missing.

## 8. CLI / config / operator surface deltas

- A workload label and a CPU-setting label, supplied by the operator (static
  config first; runtime marker later if needed). UI is out of scope
  (`docs/MEASUREMENT_GATE.md`).
- A diagnostic surface that reports which Tier 1 / Tier 2 sources are available,
  unavailable, or deferred on the current machine may be useful, mirroring
  FEAT-0004's hardware-access health signal; decided at implementation.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/cpu-work-energy-acquisition-verification-2026-06-04.md`](../cpu-work-energy-acquisition-verification-2026-06-04.md) | Feasibility input: is `read_msr` reachable? **Done (2026-06-04): yes** — the shipped `AMDFamily17.bin` already exposes `ioctl_read_msr`; APERF/MPERF + RAPL package energy are reachable read-only via the existing transport. Surfaces the 32-bit energy wrap, APERF/MPERF affinity, and INST_RETIRED-needs-writes caveats. | Current |
| [`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`](../cpu-work-energy-acquisition-decision-2026-06-07.md) | Proposed acquisition path and folded safety review: v1 = package energy (`0xC001029B` + unit `0xC0010299`) plus APERF/MPERF if affinity validates, through a read-only MSR allow-list; bin stays `warn_only` for temperature while MSR-derived fields are field-gated strict; sample id/window prevent resource-window double-counting; per-signal provenance lets energy validate independently from cycles. | Accepted 2026-06-07 |
| Counter-read Live Runtime Safety review — folded into [`cpu-work-energy-acquisition-decision-2026-06-07.md`](../cpu-work-energy-acquisition-decision-2026-06-07.md) | Confirms the read set is read-only and bounded (only `ioctl_read_msr` on a fixed MSR allow-list; never `ioctl_write_msr`), that no CPU-control write path is introduced, and how an unsupported (`#GP`) or wrapped counter degrades to blank. | Accepted — folded into the 2026-06-07 decision |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-CPUEFF-01 | T, M | unit/smoke test for work-counter delta math; runtime CSV evidence on a supported machine |
| REQ-CPUEFF-02 | T, M | unit/smoke test for energy-counter delta → Joules/avg-power; sample-id/window de-duplication; runtime CSV evidence |
| REQ-CPUEFF-03 | T, R | test context/provenance propagation and explicit deferred-signal unavailable handling; review vs `docs/RUNTIME_HOME.md` |
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
| Work proxy: instructions retired (PMC) vs delivered cycles (APERF) for v1? | the cycle-path addition | **Delivered cycles (ΔAPERF) — corrected 2026-06-09 and implemented default-off:** readable with the shipped bin at the RO alias `0xC00000E8` (the 2026-06-07 `#GP` was a probe-index error). `INST_RETIRED` needs PMC writes and stays deferred. APERF delivered-cycles is the v1 work proxy. |
| Cycle-counter MSR source (work numerator + effective frequency) | — | **Resolved (2026-06-09): no new module.** The shipped `AMDFamily17.bin` allow-lists the AMD read-only aliases `0xC00000E7`/`0xC00000E8`; reachable read-only with no new/patched bin, no signing, no repo-boundary cost. See `docs/cpu-cycle-counter-source-decision-2026-06-07.md` (resolved). Remaining: a corrected per-core-pinned live read, then the cycle path. |
| Energy source: RAPL MSR vs SMU power-reporting over SMN? | acquisition decision | RAPL MSR if reachable (monotonic energy counter); SMU mailbox is firmware-version-specific and fragile. |
| PPT/TDC/EDC + voltage source | later source decision before adding those measured fields | Deferred from v1; SMU mailbox over SMN is the likely source, but firmware-layout risk remains. |
| Where the workload/setting label lives | implementation | Static config label first; runtime marker later (shared with FEAT-0002). |
| Tier 3 (per-core/per-CCD energy+cycles, C-state/P-state residency, uncore split, EDP) | a later feature | Out of scope here; revisit after Tier 1+2 ships. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change fan channels, cadence, write timing, or
  control math. Read-only evidence.
- **Depends on:** FEAT-0002 (whole-system busy time is the time-normalization
  context); the PawnIO transport (`src/hardware/amd_reader.cpp`) and its
  availability signal (FEAT-0004, recommended operational context, not a build
  blocker).
- **Build/test impact:** counter-delta math tests, analyzer-ingest compatibility
  tests, and the enumerated-read-set review. No `CONTROL_PIPELINE_MATH.md` update
  unless control computation changes, which this feature must not do.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem stated as a named capability/contract gap: busy time is not work;
  the read path reads only SMN temperature today (§2).
- [x] 2. Stressed invariants identified — Repo Boundary, Live Runtime Safety
  (read-only, bounded register reads), runtime-home schema, Measurement Gate,
  control identity (§4).
- [x] 3. Required design decision record(s) written and marked current — the
  acquisition path and the counter-read safety review are settled by the accepted
  [`cpu-work-energy-acquisition-decision-2026-06-07.md`](../cpu-work-energy-acquisition-decision-2026-06-07.md) (§9).
- [x] 4. Concrete `REQ-CPUEFF-*` IDs confirmed (§6) and mirrored in
  `docs/TRACEABILITY.md`, now that the acquisition decision picks the v1 direction.
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [x] 6. Read-only/bounded hardware access confirmed by review and no Measurement
  Gate move — the enumerated MSR allow-list and the counter-read safety review are
  in the §9 decision (REQ-CPUEFF-05/06). Empirical confirmation on hardware is the
  live-validation acceptance item below, not a promotion gate.
- [x] 7. Doctrine check: current behavior is grounded; proposed behavior is
  labeled proposed; Tier 3 named as out of scope.

> All seven promotion gates are met, so the spec is **Accepted** (agreed and
> authorizable per `docs/features/README.md` §2). Acceptance did not by itself
> authorize the build; implementation was gated on a one-shot **read-only live MSR
> validation** on the target part (Ryzen 9 9950X3D, Family 1Ah) — RAPL
> availability/encoding on Family 1Ah, whether PawnIO honors caller thread
> affinity for `rdmsr`, and `#GP`→blank — and on the maintainer explicitly
> authorizing build work. FEAT-0004 is recommended operational context, not a
> build blocker. The committed target (Tier 1 + v1 Tier 2 context, work-per-Joule)
> and the v1 acquisition path are agreed. The executable procedure for that
> one-shot validation — read sets, the affinity-honoring test, plausibility
> bounds, and the PASS/energy-only/FAIL outcome tree — is
> [`docs/archive/implemented-plans/cpu-work-energy-live-validation-plan-2026-06-07.md`](../archive/implemented-plans/cpu-work-energy-live-validation-plan-2026-06-07.md).
>
> **Live validation ran 2026-06-07 — outcome PASS (energy-only) for the energy
> signal:** RAPL package energy is available and correctly encoded on Family 1Ah.
> (The run also reported APERF/MPERF `#GP`, but that was a probe-index error,
> corrected 2026-06-09: the shipped bin serves the AMD RO aliases
> `0xC00000E7`/`0xC00000E8`, not the architectural `0xE7`/`0xE8` the probe read —
> so effective frequency is reachable, not unavailable.) Results:
> [`docs/cpu-work-energy-live-validation-results-2026-06-07.md`](../cpu-work-energy-live-validation-results-2026-06-07.md).
> **Build authorized (energy-only) 2026-06-07** by the maintainer, after the live
> validation and the energy-only re-scope (REQ-CPUEFF-01/03 updated; the
> delivered-cycles work proxy and effective-frequency context were later added
> default-off with the shipped bin, no new module, corrected 2026-06-09, §11).
> v1 now implements the **RAPL package-energy path** plus the default-off
> **APERF/MPERF cycle path**, per
> [`cpu-work-energy-acquisition-decision-2026-06-07.md`](../cpu-work-energy-acquisition-decision-2026-06-07.md)
> §Apply order; data ships `quarantine` and is promoted to `validated` only by the
> separate post-implementation Evaluation (decision §Evaluation).

## 14. Verification log  *(fill in after the feature is built)*

> **Partial — v1 energy-logger landed 2026-06-07; analyzer average-watts
> derivation landed 2026-06-09; cycle (APERF/MPERF) logger landed 2026-06-09
> (default-off); analyzer cycle derivation landed 2026-06-10 (analyze schema
> v10).** The status stays **Accepted**
> (not yet `Implemented`). What is verified: it compiles in the full build, the
> pure math **and** the baseline/delta/sample-id/guard transition are unit-tested
> (`rapl_energy.h` / `AdvanceEnergyWindow`), the **default-off** path is inert
> (CTest 11/11 + hermetic lane via `.\scripts\Test-LocalCI.ps1`), `analyze
> report` derives time-weighted average package power from the
> `cpu_pkg_energy_*` columns — schema v9, sample-id de-duplication, no-false-zero
> "unavailable" path — covered by `ComputePackagePower` unit tests and the
> `test_analyze_ingest` end-to-end fixture, and (schema v10, 2026-06-10) the
> same report derives the cycle-weighted `ΔAPERF/ΔMPERF` ratio over distinct
> `cpu_cycles_sample_id` windows plus effective MHz when `--p0-mhz` supplies the
> base frequency (no logged field or document fixes a P0 source, so it is never
> guessed) — covered by `ComputeCpuCycles` unit tests, the e2e cycle fixture,
> the v9-DB degrade test, and the v9→v10 migration test. The **enabled**
> integration path ran live 3 times:
> session 1 on 2026-06-10 (3164 rows),
> session 2 on 2026-06-12 (5384 rows), and
> session 3 on 2026-06-14 (5384 rows). Each evidence note scored
> 5 PASS / 0 FAIL / 1 MANUAL; the MANUAL item is criterion 4
> (cycle effective-frequency validity), while the package-energy criteria passed.
> See
> [`docs/cpu-energy-quarantine-exit-evidence-2026-06-10-s1.md`](../cpu-energy-quarantine-exit-evidence-2026-06-10-s1.md),
> [`docs/cpu-energy-quarantine-exit-evidence-2026-06-12-s2.md`](../cpu-energy-quarantine-exit-evidence-2026-06-12-s2.md), and
> [`docs/cpu-energy-quarantine-exit-evidence-2026-06-14-s3.md`](../cpu-energy-quarantine-exit-evidence-2026-06-14-s3.md).
> CI still exercises only the default-off path. Still pending: the
> manual promotion decision / marker flip, the cycles-per-Joule energy↔cycle join (the
> two paths carry separate sample ids and no join rule is specified), and
> REQ-CPUEFF-08 (deferred). Promotion to `Implemented`
> additionally requires reconciling the §14 and `docs/TRACEABILITY.md`
> REQ-CPUEFF result cells to byte-identical text (then enforced by
> `test_traceability_results_match_implemented_verification_logs`).

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-CPUEFF-01 | partial (cycle logger + analyzer derivation landed; enabled live CSV sessions captured; cycle promotion pending) | Cycle logger landed: `cpu_cycles.h` pure math (allow-list {MPERF_RO `0xC00000E7`, APERF_RO `0xC00000E8`}, 64-bit modular delta, ratio + effective-freq, implausibility guard, `AdvanceCycleWindow`) + `cpu_cycles_tests`; `amd_reader` reads APERF/MPERF per-core under a transient affinity pin and logs `cpu_aperf_delta`/`cpu_mperf_delta` + `cpu_cycles_*` (default-off `SVG_MB_CONTROL_CPU_CYCLES_MODE`). Analyzer effective-frequency derivation landed 2026-06-10 (schema v10). Enabled sessions 1-3 captured `cpu_cycles_acquisition=quarantine`; evidence criterion 4 remains MANUAL, so cycle promotion and the cycles-per-Joule join remain pending. `INST_RETIRED` stays out of scope (PMC writes). | 2026-06-14 |
| REQ-CPUEFF-02 | pass (marker promotion pending) | `rapl_energy` unit tests — ESU decode, 32-bit modular wrap, multi-wrap→guard-blank, avg-watts, and the `AdvanceEnergyWindow` transition (baseline / id-increment / guard-blank-keeps-id / wrap) — + CTest via Test-LocalCI; logger landed (`amd_reader.cpp`, `runtime_csv_rows.cpp`). Analyzer landed 2026-06-09: schema v9 columns (`analyze_db`, `analyze_csv`, `analyze_ingest_db`), time-weighted `ComputePackagePower` with sample-id de-duplication + per-window watt percentiles (`analyze_report_*`), `ComputePackagePower` unit tests + `test_analyze_ingest` end-to-end (dedup over mirrored ticks, blank-delta exclusion, no-false-zero "unavailable"). Enabled sessions 1-3 captured `cpu_pkg_energy_acquisition=quarantine` and each scored 5 PASS / 0 FAIL / 1 MANUAL, with the MANUAL item limited to cycle effective-frequency validity. The unsupported fixed >=7-day span was removed 2026-06-14; energy quarantine-exit evidence is complete across 3 independent sessions. Flipping `cpu_pkg_energy_acquisition` to `validated` remains a manual maintainer decision. | 2026-06-14 |
| REQ-CPUEFF-03 | pass (v1 scope) | Temperature stays aligned with the window; energy and cycle provenance markers are independent; effective-frequency inputs are emitted only when the default-off cycle path is enabled and are derived by analyzer schema v10 rather than guessed; control-loop / `csv_rows` tests pass. | 2026-06-11 |
| REQ-CPUEFF-04 | pass | Additive nullable columns; `test_analyze_ingest` ingests subset/old archives; no-false-zero via the implausibility-guard tests. | 2026-06-07 |
| REQ-CPUEFF-05 | pass | The energy and cycle helpers issue `ioctl_read_msr` only; no `ioctl_write_msr` in the FEAT-0006 paths (code review + grep). | 2026-06-14 |
| REQ-CPUEFF-06 | pass | Enumerated read sets `{0xC0010299, 0xC001029B}` via `rapl::IsAllowlistedEnergyMsr` and `{0xC00000E7, 0xC00000E8}` via `cycles::IsAllowlistedCycleMsr`, with allow-list guard tests; field-level strict hash gate consumes the structured loader mismatch flag. | 2026-06-14 |
| REQ-CPUEFF-07 | pass | Logger records raw delta + window only; no efficiency scoring/verdict in the logger (code review). | 2026-06-07 |
| REQ-CPUEFF-08 | deferred | Workload / CPU-setting label is the shared open question with FEAT-0002 §8; not implemented in v1. | 2026-06-07 |

**Spec vs. implementation deltas:** v1 now lands the RAPL package-energy path and
the APERF/MPERF delivered/reference-cycle path, both default-off and read-only.
`INST_RETIRED` remains out of scope because it needs PMC writes, and the
cycles-per-Joule join is not specified because the energy and cycle paths carry
separate sample ids. The energy read rides **AmdReader's own ~1 s cadence in the
snapshot path** (it owns the PawnIO handle), not literally adjacent to
`GetSystemTimes` in `tick_runner` as the decision §Apply order sketched — same
~1 s, 1-read/s disturbance profile, read outside the `Global\Access_PCI` mutex.
Average watts and effective frequency are derived (analyzer/manual), not logged.
Quarantine promotion remains gated on the multi-session Evaluation.
