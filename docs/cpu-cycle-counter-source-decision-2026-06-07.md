# CPU Cycle-Counter MSR Source Decision - 2026-06-07

Status: **resolved 2026-06-09 — no new module needed.** This was raised by the
FEAT-0006 live validation as an open question ("is a cycle-counter-capable
PawnIO module available/packageable?"). The answer is **the shipped
`AMDFamily17.bin` already services the cycle counters** — the 2026-06-07
"`#GP` → unreachable → need a new module" reading was a **probe-index error**, not
a module limitation. No new/patched module, no signing, and no `AGENTS.md`
§Repo Boundary cost. This record settles the source question; it does **not**
authorize the build (the maintainer authorizes that) and it does **not** promote
any data out of quarantine.

**Companion to:**
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md` (§11 open
decision),
`docs/cpu-work-energy-live-validation-results-2026-06-07.md` (the evidence that
raised this, §Finding 2 corrected),
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`,
`third_party/pawnio/README.md`, `AGENTS.md` §Repo Boundary.

## What the 2026-06-07 probe actually showed (and the error)

The 2026-06-07 gate probe read the **architectural** indices `IA32_MPERF 0xE7`
/ `IA32_APERF 0xE8` (and `TSC_AUX 0xC0000103`) and got `ACCESS_DENIED`, then
concluded the shipped bin "filters" the cycle counters and a new module would be
needed.

`AMDFamily17.p` does not allow-list those indices — it allow-lists the AMD
**read-only aliases** instead:

```
#define MSR_MPERF_RO   (0xC00000E7)
#define MSR_APERF_RO   (0xC00000E8)
...
bool:is_allowed_msr_read(msr) {
    switch (msr) {
        case MSR_AMD64_PATCH_LEVEL, MSR_MPERF_RO, MSR_APERF_RO, ... : return true;
```

`ioctl_read_msr` returns `STATUS_ACCESS_DENIED` for any index not on that list,
so reading `0xE7`/`0xE8` fails while `0xC00000E7`/`0xC00000E8` succeed. The probe
never tried the RO aliases. (Verified in both the vendored **0.2.6** source — the
bin we ship — and the latest **0.2.7** source; namazso/PawnIO.Modules.)

## Resolution

A first-party CPU **work numerator** and **effective frequency** are obtainable
with the bin **already shipped**, by reading the AMD read-only aliases:

- `MSR_MPERF_RO 0xC00000E7` — Read-Only Max Performance Frequency Clock Count;
  counts at the P0 frequency while the core is in C0.
- `MSR_APERF_RO 0xC00000E8` — Read-Only Actual Performance Frequency Clock Count;
  counts in proportion to the actual core clock while in C0.
- **Effective frequency = (ΔAPERF / ΔMPERF) × P0 frequency**; **delivered cycles
  (the work numerator) = ΔAPERF.** (AMD OSRR for Family 17h, doc 56255.)

Both are 64-bit, read-only, per-core, and already inside the module's read
allow-list — so REQ-CPUEFF-05/06 (read-only, enumerated allow-list) are satisfied
by the **existing** vendored bin with no change. The "vendor a new/patched bin +
hash-pin + signing" cost this stub was created to weigh **does not apply.**

## What remains to deliver the work numerator in v1

This is no longer a separate v2 source decision; it is a near-term addition to
the FEAT-0006 energy-only v1, gated on the same discipline the energy path used:

1. **Corrected one-shot live read (read-only).** Read `0xC00000E7`/`0xC00000E8`
   on the target part under a **per-core affinity pin**, confirming the reads
   succeed and that ΔAPERF/ΔMPERF yields a plausible effective frequency that
   tracks load (idle vs busy). This answers the original Q1 (does PawnIO honor
   caller affinity for `rdmsr`?) and Q3 (plausible effective frequency?) that the
   2026-06-07 run wrongly treated as moot. Harness:
   `tools/cpu_cycle_counter_probe.cpp` (throwaway, read-only, default-off
   `SVG_MB_CONTROL_BUILD_CYCLE_PROBE`). Run elevated, controller monitor-only.
2. **Implement the cycle path** by mirroring the energy path: extend the
   read-only MSR allow-list to the two RO aliases, derive ΔAPERF/ΔMPERF over the
   existing `cpu_power_sample_id` windows, log raw deltas (additive nullable
   columns + a `cpu_cycles_acquisition` provenance marker), default-off.
3. **Quarantine + Evaluation.** The cycle data ships `quarantine`; the
   effective-frequency validity criterion (decision §Evaluation #4 — plausible
   clock, stable under a held affinity) plus the per-signal Evaluation promote it
   to `validated` independently of energy.

## Constraints (all satisfied by the existing bin)

- Read-only, fixed allow-list, no CPU-control write (FEAT-0006 REQ-CPUEFF-05/06):
  `ioctl_read_msr` on `0xC00000E7`/`0xC00000E8` only; both are read-only aliases.
- Repo Boundary: **no new bin** — the vendored, SHA-pinned `AMDFamily17.bin`
  (PawnIO.Modules 0.2.6) already exposes the reads. (0.2.7 is available and
  carries the same allow-list, but no upgrade is required.)
- Strict-hash gate for MSR-derived fields already exists (the energy path's
  field-level gate covers any RO-alias-derived field too).

## Status / gating

**Resolved.** The cycle path is a near-term FEAT-0006 **v1** addition (not a
separate v2 module slice). It is gated only on the corrected one-shot live read
above and the maintainer's build authorization — not on any new module, source,
or signing decision.
