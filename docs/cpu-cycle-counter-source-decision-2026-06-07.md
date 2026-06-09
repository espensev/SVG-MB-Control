# CPU Cycle-Counter MSR Source Decision - 2026-06-07

Status: **open / proposed stub** — framing only, not decided, not scheduled. This
records the question raised by the FEAT-0006 live validation so it has a home; it
settles nothing yet and authorizes no work.

**Companion to:**
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md` (§11 open
decision),
`docs/cpu-work-energy-live-validation-results-2026-06-07.md` (the evidence that
raised this),
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`,
`third_party/pawnio/README.md`, `AGENTS.md` §Repo Boundary.

## Problem

The FEAT-0006 pre-implementation live validation (2026-06-07) found that the
shipped PawnIO `AMDFamily17.bin` services `ioctl_read_msr` for the RAPL energy
MSRs (`0xC0010299`, `0xC001029B`) but **`#GP`s on the cycle counters** APERF
(`0xE8`) / MPERF (`0xE7`) and on TSC_AUX (`0xC0000103`), on the same read path —
an index-specific rejection (likely an upstream module read-set restriction, not
confirmable from the vendored `.bin`).

Consequence: FEAT-0006 v1 is **energy-only**. It has **no first-party work
numerator** (delivered cycles unreadable; `INST_RETIRED` needs PMC writes, out of
read-only scope), so first-party **work-per-Joule** — the §2 motivation — is not
computable, and effective frequency is unavailable. Energy/package power (cooling
watts-per-RPM) still lands.

## Question to settle (when scheduled)

Is there a PawnIO module whose `read_msr` actually services the cycle/instruction
counters — **APERF `0xE8` / MPERF `0xE7` first** (the work numerator + effective
frequency), and ideally a retired-instructions counter — reachable read-only
through the existing transport, **without** breaking `AGENTS.md` §Repo Boundary?

## Directions to explore (none chosen)

- A different or newer upstream PawnIO module (namazso `PawnIO.Modules`) whose
  read set includes the cycle counters; verify with a throwaway read-only probe
  of the same shape as the 2026-06-07 gate probe (since removed; its shape is
  recorded in the validation plan §9) before trusting.
- A custom/patched `.p` module exposing a read-only, allow-listed cycle-counter
  read. Cost: building, hash-pinning, and vendoring a new bin
  (`src/hardware/pawnio_binary.cpp` `kPawnIoSpec*`), plus the strict-hash gate for
  MSR-derived fields.
- Confirm whether the rejection is a module read-set restriction vs. something
  else (upstream `AMDFamily17.p` source review).

## Constraints

- Read-only, fixed allow-list, no CPU-control write (carries FEAT-0006
  REQ-CPUEFF-05/06).
- Repo Boundary: a new/patched bin must be vendored bit-identical with a pinned
  SHA-256, not fetched at runtime or bridged from a sibling repo.
- Any candidate is validated live (read-only) with the same one-shot probe
  approach before any field is trusted, and still enters quarantine.

## Status / gating

Open and unscheduled. It gates any FEAT-0006 **v2** slice that would add a
first-party work numerator or effective frequency; it does **not** block the
energy-only v1.
