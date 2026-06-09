# CPU Work/Energy Live MSR Validation Results - 2026-06-07

Status: **results** of the one-shot, read-only live MSR validation defined by
`docs/cpu-work-energy-live-validation-plan-2026-06-07.md` (the FEAT-0006
pre-implementation gate). Run once on the target part, read-only, with the
controller in monitor-only mode (not driving fans). **Outcome: PASS
(energy-only).**

This records what the live read found; it does **not** promote any data to
`validated` (that is the separate post-implementation quarantine-exit
Evaluation, `cpu-work-energy-acquisition-decision-2026-06-07.md` §Evaluation),
and it does **not** authorize the build (the maintainer authorizes that
separately).

**Companion to:**
`docs/cpu-work-energy-live-validation-plan-2026-06-07.md`,
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
| Q3 | APERF/MPERF → plausible effective frequency? | **Not available.** `0xE7`/`0xE8` `#GP` through the shipped module. |
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

2. **APERF/MPERF are NOT reachable through the shipped bin (empirical).**
   `0xE7`/`0xE8` (and the earlier-tried `0xC0000103` TSC_AUX) `#GP`/error, while
   the RAPL MSRs on the **same** `ReadMsrRaw` / buffer / `DeviceIoControl` path
   succeed (returning the documented ESU and load-tracking power) and the
   deliberate bogus read errors as expected. So the rejection is index-specific
   at the driver, not a probe/transport artifact. **Likely cause** (inferred, not
   confirmed): the upstream `AMDFamily17.p` module restricts `ioctl_read_msr` to
   a subset of indices that includes the RAPL energy MSRs but excludes the
   architectural cycle counters — the vendored files
   (`third_party/pawnio/README.md`, the `.bin`) document the read function but
   not any per-index restriction, so the mechanism is not verifiable from
   in-repo source. Either way, the **observable result** is what matters:
   APERF/MPERF do not read through this bin. This **falsifies** the verification
   doc's §Per-signal feasibility row that listed APERF/MPERF as reachable "via
   `ioctl_read_msr`" — reachability was inferred from the module *exposing* the
   read function and never tested live.

3. **`#GP` degrades to blank cleanly** (no crash, no false zero), confirming the
   decision doc §Counter-read safety review behavior on this driver.

## Outcome and implications

**PASS (energy-only).** v1 acquires **RAPL package energy only**. The affinity
question (Q1) is moot for v1 because the cycle counters are unreadable, not
merely affinity-untrusted.

- This lands in the **energy-only branch the acquisition decision already
  anticipated** ("otherwise v1 ships energy-only and effective frequency stays
  withheld"), so the committed design does not need to be reopened to proceed —
  but the *reason* is stronger than expected: effective frequency is not a
  "validate affinity later" item, it is **not obtainable with the current bin
  at all**.
- **Recovering effective frequency is now a module problem, not an affinity
  problem.** It would require a PawnIO module whose `read_msr` actually services
  `0xE7`/`0xE8` (a different or patched bin), which is a separate source decision
  touching `AGENTS.md` §Repo Boundary (vendoring/packaging a new module) — not in
  this slice.
- **Requirement impact:** REQ-CPUEFF-01 (a first-party **work** signal —
  instructions retired and/or delivered cycles) has **no satisfying first-party
  signal in v1**: delivered cycles (APERF) are filtered by the bin and
  `INST_RETIRED` needs PMC writes. It is **deferred, not met**. Energy is the
  *denominator* (REQ-CPUEFF-02), not a work signal — so **first-party
  work-per-Joule (the §2 motivation) is not computable in v1**. What energy-only
  v1 does deliver: package power = heat dissipated (cooling watts-per-RPM), and
  score-per-Joule when an external fixed-workload score supplies the work term.
  REQ-CPUEFF-03's effective-frequency context is `unavailable` in v1 (correctly,
  not guessed). These are maintainer-authored spec/decision re-scopes to make
  before the energy-only build is scoped, separate from this results record.

## Next steps

1. Maintainer decides whether to (a) authorize an **energy-only v1** build
   (RAPL package energy, default-off, quarantined), and/or (b) open a separate
   source decision for an APERF/MPERF-capable PawnIO module to recover effective
   frequency later.
2. If (a): implement the energy path per the decision doc §Apply order; the data
   ships `quarantine` and only the post-implementation Evaluation (≥ 3 sessions /
   ≥ 7 days, ±15% external cross-check) promotes it to `validated`.
3. **Done.** The throwaway probe (`tools/cpu_msr_validation_probe.cpp`) and its
   `SVG_MB_CONTROL_BUILD_MSR_PROBE` build option were removed when this record was
   accepted and the energy-only slice landed. It was never committed (throwaway);
   its method and output stay recorded here (§Setup, the raw probe output above)
   and in the validation plan §9.
