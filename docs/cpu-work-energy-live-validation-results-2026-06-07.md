# CPU Work/Energy Live MSR Validation Results - 2026-06-07

Status: **results** of the one-shot, read-only live MSR validation defined by
`docs/archive/implemented-plans/cpu-work-energy-live-validation-plan-2026-06-07.md` (the FEAT-0006
pre-implementation gate). Run once on the target part, read-only, with the
controller in monitor-only mode (not driving fans). **Outcome: PASS
(energy-only).**

This records what the live read found; it does **not** promote any data to
`validated` (that is the separate post-implementation quarantine-exit
Evaluation, `cpu-work-energy-acquisition-decision-2026-06-07.md` §Evaluation),
and it does **not** authorize the build (the maintainer authorizes that
separately).

> **⚠ CORRECTION (2026-06-09).** Finding 2 and the effective-frequency
> conclusions in this record are **wrong**. The probe read the architectural
> indices `IA32_MPERF 0xE7` / `IA32_APERF 0xE8` (and `TSC_AUX 0xC0000103`), which
> `AMDFamily17.p` does not allow-list, so they returned `ACCESS_DENIED`. The
> shipped bin **does** service the cycle counters — at the AMD read-only aliases
> `MSR_MPERF_RO 0xC00000E7` / `MSR_APERF_RO 0xC00000E8` (verified in the 0.2.6
> source we ship and 0.2.7; AMD OSRR 56255). So the work numerator (ΔAPERF) and
> effective frequency (ΔAPERF/ΔMPERF × P0) **are obtainable with the bin already
> shipped** — no new module. The **energy** results below stand; the
> "energy-only, cycle counters need a new module" conclusion does not. Resolved
> in `docs/cpu-cycle-counter-source-decision-2026-06-07.md`. The raw probe output
> is kept verbatim as the record of what was (mis-)tested.

**Companion to:**
`docs/archive/implemented-plans/cpu-work-energy-live-validation-plan-2026-06-07.md`,
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`,
`docs/cpu-work-energy-acquisition-verification-2026-06-04.md`.

## Setup

- **Part:** AMD Ryzen 9 9950X3D, `family 0x1A (26)` = Family 1Ah / Zen 5,
  `model 0x44 (68)` — confirmed by the probe's own CPUID line.
- **Transport:** shipped `resources/pawnio/AMDFamily17.bin`, loaded `warn_only`,
  hash matched (no load warning).
- **Tool:** `tools/cpu_msr_validation_probe.cpp` (throwaway, read-only,
  `ioctl_read_msr` only; since removed — see §Next steps). Run elevated,
  controller monitor-only.

## Per-question results

| # | Question | Result |
|---|---|---|
| reach | Which allow-list MSRs does the module service? | `pwr_unit=ok pkg_energy=ok aperf=#GP mperf=#GP` |
| Q2 | RAPL available + ESU encoded as documented on Family 1Ah? | **PASS.** `0xC0010299` ESU=16 → 15.2588 µJ/unit (the documented default). Energy tracked load: baseline 58.9 W → all-core busy 111.7 W, monotonic, below the 400 W ceiling. |
| Q1 | PawnIO honors caller affinity for `rdmsr`? | **Moot.** APERF/MPERF are not readable through this bin (below), so the cycle path does not exist in v1 regardless of affinity. |
| Q3 | APERF/MPERF → plausible effective frequency? | **Untested — wrong index (corrected 2026-06-09).** `0xE7`/`0xE8` `#GP` because the module allow-lists the AMD RO aliases `0xC00000E7`/`0xC00000E8`, which were not read. Reachable; a corrected per-core-pinned read is pending. |
| Q4 | Absent/unsupported MSR → blank, no crash/false-zero? | **PASS.** A deliberate `0xFFFFFFFF` read returned an error (blank); no crash, no zero. |

### Raw probe output

```
== FEAT-0006 CPU MSR validation probe (read-only, one-shot) ==
plan: docs/cpu-work-energy-live-validation-plan-2026-06-07.md
cpu:  AMD Ryzen 9 9950X3D 16-Core Processor           [AuthenticAMD] family 0x1A (26) model 0x44 (68)

bin:  ...\resources\pawnio\AMDFamily17.bin
load: ok

[reachable]    pwr_unit=ok pkg_energy=ok aperf=#GP mperf=#GP

[Q2 RAPL unit] 0xC0010299 raw=0x00000000000a1000 ESU=16 uj/unit=15.2588 -> plausible
[Q2 energy]    idle=58.9 W  busy=111.7 W  (ceiling 400 W) -> plausible+tracks-load
[Q1 affinity]  skipped: APERF unreachable (PawnIO filters 0xE8) -> effective frequency unavailable
[Q3 eff-freq]  skipped (affinity not honored, or APERF/MPERF unreachable)
[Q4 fault]     read 0xFFFFFFFF -> error(blank) -> ok (no crash, no false zero)

== OUTCOME: PASS (energy-only) ==
```

## Findings

1. **RAPL package energy works on Family 1Ah (the headline risk is resolved
   positively).** `0xC0010299`/`0xC001029B` read cleanly; the energy unit
   decodes to the documented default (ESU=16, 15.26 µJ/unit); 32-bit
   modular-differenced power is positive, below the ceiling, and rises and falls
   with load. The Family-1Ah RAPL availability/encoding caveat carried since the
   verification doc is cleared for the energy signal.

2. **APERF/MPERF reachability — this conclusion was WRONG (corrected
   2026-06-09).** The probe read `0xE7`/`0xE8` (and `0xC0000103` TSC_AUX); all
   returned `ACCESS_DENIED` while the RAPL MSRs on the same path succeeded —
   correctly observed as an index-specific driver rejection. The error was the
   **inference** that the counters are therefore unreachable. The upstream
   `AMDFamily17.p` source (the **0.2.6** bin we ship, and **0.2.7**) allow-lists
   `ioctl_read_msr` for the AMD **read-only aliases** `MSR_MPERF_RO 0xC00000E7` /
   `MSR_APERF_RO 0xC00000E8` — **not** the architectural `0xE7`/`0xE8`. So the
   probe read the wrong indices; APERF/MPERF (and thus the work numerator and
   effective frequency) **are reachable through this very bin** at the RO
   aliases. This does **not** falsify the verification doc's reachability row —
   they are reachable; only the index to read on AMD via this module differs. See
   `docs/cpu-cycle-counter-source-decision-2026-06-07.md` (resolved) and AMD OSRR
   56255.

3. **`#GP` degrades to blank cleanly** (no crash, no false zero), confirming the
   decision doc §Counter-read safety review behavior on this driver.

## Outcome and implications

**PASS (energy-only) for the energy signal.** As implemented, v1 acquires **RAPL
package energy only**. (Corrected 2026-06-09: the cycle counters are **not**
unreadable — they read at the AMD RO aliases `0xC00000E7`/`0xC00000E8`; the
affinity question Q1 is therefore live again, not moot, and is part of the
corrected cycle-path re-validation.)

- This lands in the **energy-only branch the acquisition decision already
  anticipated** ("otherwise v1 ships energy-only and effective frequency stays
  withheld"), so the committed design does not need to be reopened to proceed —
  and (corrected 2026-06-09) the "stronger reason" recorded here — that
  effective frequency was *not obtainable with the current bin at all* — was
  itself wrong: the probe read the wrong indices (see the correction above).
- **Recovering effective frequency needs no new module (corrected 2026-06-09).**
  The shipped bin already allow-lists the AMD read-only aliases
  `0xC00000E7`/`0xC00000E8`; the cycle path is a near-term addition to v1 gated on
  a corrected per-core-pinned live read, not a separate source/module decision.
  See `docs/cpu-cycle-counter-source-decision-2026-06-07.md`.
- **Requirement impact (corrected 2026-06-09):** REQ-CPUEFF-01 (a first-party
  **work** signal) is **achievable in v1**, not blocked: delivered cycles
  (ΔAPERF) are readable via the RO alias `0xC00000E8` with the shipped bin
  (`INST_RETIRED` still needs PMC writes and stays out of read-only scope, but it
  is not required — APERF delivered-cycles is the v1 work proxy per FEAT-0006
  §11). So first-party **work-per-Joule** (the §2 motivation) **is computable in
  v1** once the cycle path is implemented. As **currently implemented**, v1 is
  still energy-only (the cycle path is not yet built); package power = heat
  dissipated and score-per-Joule (external score) are what the shipped logger
  delivers today.
  REQ-CPUEFF-03's effective-frequency context is `unavailable` in v1 (correctly,
  not guessed). These are maintainer-authored spec/decision re-scopes to make
  before the energy-only build is scoped, separate from this results record.

## Next steps

1. **(a) Done** — the energy-only v1 build (RAPL package energy, default-off,
   quarantined) was authorized and landed (commit 3d334d7). **(b) Resolved, not
   needed (corrected 2026-06-09)** — no separate module source decision is
   required: the shipped bin already serves APERF/MPERF at the RO aliases. Adding
   the work numerator / effective frequency now needs only a corrected
   per-core-pinned live read (`tools/cpu_cycle_counter_probe.cpp`) then the cycle
   path, per `docs/cpu-cycle-counter-source-decision-2026-06-07.md`.
2. If (a): implement the energy path per the decision doc §Apply order; the data
   ships `quarantine` and only the post-implementation Evaluation (≥ 3
   independent sessions, ±15% external cross-check) promotes it to `validated`.
3. **Done.** The throwaway probe (`tools/cpu_msr_validation_probe.cpp`) and its
   `SVG_MB_CONTROL_BUILD_MSR_PROBE` build option were removed when this record was
   accepted and the energy-only slice landed. It was never committed (throwaway);
   its method and output stay recorded here (§Setup, the raw probe output above)
   and in the validation plan §9.

## Corrected re-validation (2026-06-09) — APERF/MPERF reachable, PASS

Run with the corrected probe (`tools/cpu_cycle_counter_probe.cpp`) reading the
AMD read-only aliases, elevated, controller monitor-only, on the same part
(Ryzen 9 9950X3D, Family 1Ah).

```
== FEAT-0006 cycle-counter probe (read-only, corrected indices) ==
bin:  ...\resources\pawnio\AMDFamily17.bin
load: ok
[sanity] pkg_energy   0xC001029B: ok
[reach]  MPERF_RO     0xC00000E7: ok
[reach]  APERF_RO     0xC00000E8: ok
[reach]  IA32_MPERF   0x000000E7: DENIED (expect DENIED)
[reach]  IA32_APERF   0x000000E8: DENIED (expect DENIED)
[affinity] pinned to core 0 (confirmed)
[idle]  dMPERF=745832893 dAPERF=961169284  APERF/MPERF=1.289
[busy]  dMPERF=2145984445 dAPERF=2746900132  APERF/MPERF=1.280
```

**Result: PASS.**

- **Reachability (the correction):** `MSR_MPERF_RO 0xC00000E7` / `MSR_APERF_RO
  0xC00000E8` read `ok`; the architectural `0xE7`/`0xE8` are `DENIED` — confirming
  live that the 2026-06-07 `#GP` was a wrong-index artifact, not a module limit.
- **Affinity (original Q1):** the read pinned to core 0 (confirmed) and returned
  large, monotonic per-core deltas with a stable ratio across both samples —
  consistent with PawnIO honoring caller affinity for `rdmsr` (an unhonored
  affinity would straddle cores and yield an erratic/implausible ratio).
- **Plausible effective frequency (original Q3):** ΔAPERF/ΔMPERF ≈ 1.28–1.29 →
  effective ≈ 1.28 × P0 (~5.5 GHz at the 4.3 GHz base), within [idle, rated boost
  ~5.7 GHz] and stable. Load tracking shows in the **MPERF C0-residency**, not the
  ratio: idle ΔMPERF ≈ 0.75e9 (~35% of a 4.3 GHz × 0.5 s window) vs busy ΔMPERF ≈
  2.15e9 (~full C0). The ratio (effective multiplier) is ~boost in both because
  the core boosts whenever active; the load signal is the residency.

**Implication:** the work numerator (ΔAPERF = delivered cycles) and effective
frequency (ΔAPERF/ΔMPERF × P0) are read-only reachable and **live-confirmed** with
the **shipped** bin. The cycle path is unblocked for implementation (mirror the
energy path); cycle data, once logged, still ships `quarantine` and is promoted
only by the per-signal Evaluation (decision §Evaluation #4).
