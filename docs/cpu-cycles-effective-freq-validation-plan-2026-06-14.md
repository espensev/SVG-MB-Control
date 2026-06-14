# CPU Cycles / Effective-Frequency Validation Plan — 2026-06-14

Status: plan only. Defines the evidence that would move `cpu_cycles_acquisition`
from `quarantine` toward a maintainer promotion (decision doc
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` §Evaluation criterion
4). No code change and no marker change here.

This plan is separate from the package-energy marker, which is already covered by
`docs/cpu-energy-quarantine-exit-validation-2026-06-14.md` and does not depend on
the cycle path.

## What is already established

- The per-core APERF/MPERF read path is reachable on Family 1Ah through the
  shipped AMD read-only aliases `0xC00000E7` (MPERF_RO) / `0xC00000E8`
  (APERF_RO), under held affinity (live probe 2026-06-09).
- The cycle logger ships default-off (`SVG_MB_CONTROL_CPU_CYCLES_MODE`) and emits
  `cpu_aperf_delta` / `cpu_mperf_delta`.
- Sessions 1-3 captured those deltas with `cpu_cycles_acquisition=quarantine`.
  The `dAPERF/dMPERF` ratio is consistent across sessions: idle ~= 1.23,
  load ~= 1.19.
- The analyzer (schema v10) derives effective MHz as `ratio x P0` only when
  `--p0-mhz <base>` is supplied; without it the ratio is reported but not the MHz.

## What is unconfirmed (criterion 4)

The ratio is plausible but has no independent cross-check. The package-energy
cross-check (criterion 3) says nothing about frequency. Two failure modes remain
open:

1. **Affinity not honored by the enabled path.** If the paired APERF/MPERF reads
   land on different cores, the ratio is a cross-core artifact. The 2026-06-09
   probe pinned affinity and saw a plausible ratio, but the enabled logger path
   has not been cross-checked against an independent effective-clock value.
2. **Base-frequency / scaling error.** `ratio x P0` assumes the supplied `--p0-mhz`
   is the correct multiplier. A wrong base yields a wrong effective MHz that is
   still inside the plausible boost band.

## Validation options

### Option A — effective-clock cross-check (lowest effort)

Over the same steady load window used for the energy cross-check, record per-core
"Effective Clock" from HWiNFO (or Ryzen Master). Acceptance: the analyzer's
cycle-derived effective MHz agrees within +/-10% with the external effective
clock over the steady window.

Caveat: HWiNFO "Effective Clock" is itself APERF/MPERF-derived, so this is an
encoding/affinity cross-check (does our paired read plus math match a known-good
APERF/MPERF consumer) rather than a physically independent frequency
measurement — the same shared-source relationship the energy side has between
RAPL and the SMU.

### Option B — locked-frequency cross-check (independent reference)

Lock one core to a known fixed P-state (boost disabled), drive it to full
residency, and read the cycle-derived effective MHz over a steady window.
Acceptance: derived effective MHz equals the locked setpoint within +/-X%
(propose +/-5%). The reference here is the BIOS/OS frequency setpoint, which is
independent of any APERF/MPERF reporter, so this is the option that actually
tests `ratio x P0` against ground truth rather than against another consumer of
the same counters. Recommended if Option A agreement is to be trusted as more
than a consistency check.

## Selected approach (2026-06-14)

Option B (locked-frequency cross-check) is selected as the criterion-4 validation
method: it tests `ratio x P0` against the BIOS/OS frequency setpoint, a reference
independent of the APERF/MPERF counters. Option A may be run first as a quick
consistency pass but is not sufficient on its own, because HWiNFO/Ryzen Master
effective clock is itself APERF/MPERF-derived.

Execution requires locking a representative core to a fixed P-state (boost
disabled) on the test machine and is a manual, operator-run step. It is not
performed automatically and does not change the live controller (the cycle path
stays default-off).

## Separate open item: cycles-per-Joule join

Effective frequency and package energy are logged under different sample windows
(`cpu_power_sample_id` for energy vs the cycle sample window). There is no join
rule, so a work-per-Joule (cycles/J) figure cannot be derived yet. Defining the
join — a shared window key or an explicit interpolation rule — is a prerequisite
for the FEAT-0006 efficiency conclusion and is tracked separately from criterion-4
promotion. A fixed-work microbenchmark (known retired-instruction count) is the
natural source for the work numerator once the join exists, but it is out of
scope for criterion 4.

## Out of scope

- No control use (read-only, log-only; decision doc §Scope).
- `INST_RETIRED` is not in the read-only allow-list and is not added here.
