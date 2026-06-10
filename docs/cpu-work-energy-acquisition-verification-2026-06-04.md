# CPU Work/Energy Acquisition — Feasibility Verification - 2026-06-04

Status: verification findings for
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`. This is
evidence-gathering input to FEAT-0006 §9 (the acquisition decision and the
counter-read safety review). It is **not** an authorization to read MSRs live:
no live MSR read was performed for this verification, and none must be until the
safety review (FEAT-0006 REQ-CPUEFF-06) is written and marked current. The
2026-06-07 acquisition decision later folded in that safety review; the one-shot
read-only live MSR validation that was the remaining pre-implementation gate ran
2026-06-07 (PASS, energy-only) — see §Follow-up status.

**Companion to:** `docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
`third_party/pawnio/README.md`, `docs/features/FEAT-0004-hardware-access-health-signal.md`.

## Question

The FEAT-0006 lead direction is: *verify whether a PawnIO module exposing
`read_msr` is available or packageable for the target machines*, so APERF/MPERF
(effective frequency / delivered cycles) and AMD RAPL package energy can be read
through the existing driver transport.

## Verdict

**`ioctl_read_msr` is already exposed by the PawnIO module this repo ships.** No
new or different module is required to reach the Tier 1/Tier 2 MSR signals. The
read path is reachable through the existing transport with no driver change and
no transport-layer code change.

The remaining work is not "get a capable module." At the time of this
verification, the follow-up design items were: (a) a safety review of the exact
read set, (b) handling two counter-format caveats (32-bit energy wraparound;
per-core APERF/MPERF affinity), and (c) deciding the work proxy (delivered
cycles vs instructions retired). Those design items were later settled by
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`; live validation
remains.

## Evidence

1. **The shipped bin exports MSR reads.** `third_party/pawnio/README.md` §"IOCTL
   Functions Exposed By `AMDFamily17.bin`" lists, from upstream `AMDFamily17.p`:
   - `ioctl_read_msr` (1 input = MSR index, 1 output = value)
   - `ioctl_write_msr` (2 inputs, 0 outputs)
   - `ioctl_read_smn` (1 input, 1 output)

   The bin is upstream PawnIO.Modules `0.2.6`, vendored bit-identical at
   `resources/pawnio/AMDFamily17.bin` with a pinned SHA-256 in
   `kPawnIoSpecAmdFamily17V1` (`src/hardware/pawnio_binary.cpp:136-141`).

2. **The transport is generic and already proven.** `ExecutePawnIo`
   (`src/hardware/amd_reader.cpp:212-255`) dispatches any function name with up to
   four `int64` inputs and N `int64` outputs via one `DeviceIoControl`. Today it
   is called only as `ExecutePawnIo(handle, "ioctl_read_smn", &addr, 1, &out, 1)`
   (`amd_reader.cpp:509-510`). An MSR read is the same call with
   `"ioctl_read_msr"` and an MSR index — no transport change.

3. **The codebase already anticipated MSR-reading modules.**
   `PawnIoVerification` (`src/hardware/pawnio_binary.h:24-36`) documents `strict`
   as *"Required for any bin that exposes MSR reads: a silently swapped bin could
   lie about counter contents,"* and `warn_only` as the mode for *"the legacy
   SMN-only bin."* The bin is currently loaded `warn_only`
   (`pawnio_binary.cpp:140`).

4. **Target CPU and MSR availability.** The development machine is an
   **AMD Ryzen 9 9950X3D**, reported `AMD64 Family 26 Model 68` =
   **Family 1Ah (Zen 5), model 0x44** (`Get-CimInstance Win32_Processor`). The
   target MSRs exist on AMD Zen (Family 17h and later, including 1Ah):
   - `IA32_MPERF` `0xE7`, `IA32_APERF` `0xE8` — architectural; per logical
     processor; only the ΔAPERF/ΔMPERF **ratio** is architecturally defined, used
     as `effective_freq = (ΔAPERF/ΔMPERF) × reference`. As architectural MSRs
     these are the most portable; present on this part.
   - `MSR_RAPL_PWR_UNIT` `0xC0010299` — socket-shared energy/power/time units
     (energy unit `1/2^ESU` J; ESU default `0b10000` → 15.3 µJ/unit).
   - `MSR_CORE_ENERGY_STAT` `0xC001029A` — per-core 32-bit energy accumulator.
   - `MSR_PKG_ENERGY_STAT` `0xC001029B` — socket-level 32-bit energy accumulator,
     shared across cores. Updated ~every 1 ms; cleared on reset.

   The RAPL addresses are documented and sourced for **AMD Zen Family 17h and
   later**; the sources reviewed for this verification confirm 17h+ but do not
   explicitly enumerate Family 1Ah (Zen 5). The exact RAPL availability and
   encoding on this 9950X3D are therefore **to be confirmed in live validation**,
   not asserted here. This caution is concrete, not pro-forma: the existing code
   already treats this chip (model `0x44`) through a **Zen 4** CCD base in
   `SelectCcdLayout` (`amd_reader.cpp:311-326`), so register layouts on this part
   are demonstrably family-sensitive.

## Per-signal feasibility

| FEAT-0006 need | Source | Reachable via shipped bin? | Caveat |
|---|---|---|---|
| Effective frequency / delivered cycles (Tier 1/2) | APERF `0xE8`, MPERF `0xE7` (read-only, free-running) | ~~Yes — `ioctl_read_msr`~~ **superseded 2026-06-07: live `#GP`** (see results doc) | **Per-core, and the ratio can be invalid, not merely coarse.** `ioctl_read_msr` takes only an MSR index (no affinity input), so two separate calls can be serviced on different cores; a ΔAPERF/ΔMPERF ratio whose reads straddle a core migration is meaningless. The standard fix is caller-side `SetThreadAffinityMask` before the IOCTL (no module change) — **but only if PawnIO executes the `rdmsr` on the calling thread** rather than marshaling to its own context. Which is true is unconfirmed and must be validated before trusting effective frequency (live-validation item). |
| Package energy → Joules + avg power (Tier 1) | `MSR_PKG_ENERGY_STAT` `0xC001029B` + unit `0xC0010299` | Likely — `ioctl_read_msr` (RAPL availability on Family 1Ah to confirm) | **32-bit accumulator → wraps.** At 15.3 µJ/unit, 2^32 units ≈ 65.7 kJ (~5–6 min at 200 W). Delta math must use 32-bit modular subtraction, unlike the 64-bit CPU-time counters. Package-scoped: any core returns the socket total, so no affinity issue. |
| Instructions retired (Tier 1, "ideal" work) | PMC (`PERF_CTL`/`PERF_CTR`, AMD `0xC0010000`+) | **No, not read-only.** Programming a PMC requires `ioctl_write_msr`. | Collides with REQ-CPUEFF-05 (read-only). Defer; use APERF delivered-cycles as the read-only work proxy for v1 (matches FEAT-0006 §11 default). |
| Vcore / VID (Tier 2) | SVI2/SVI3 telemetry / SMU — not a plain architectural MSR | Not via these MSRs | Needs SMU/SMN path; best-effort, firmware-specific (FEAT-0006 §11). |
| Throttle / limit reason PPT/TDC/EDC (Tier 2) | AMD SMU power-reporting (mailbox over SMN) | Not via MSR | SMU mailbox is firmware-version-specific and fragile (FEAT-0006 §11). |
| Per-core energy (Tier 3) | `MSR_CORE_ENERGY_STAT` `0xC001029A` | Yes — `ioctl_read_msr` | Per-core → same affinity caveat; Tier 3, out of committed scope. |

## Key risks and required safeguards

1. **The same bin also exposes `ioctl_write_msr`.** This repo must never call it.
   The safety review must assert that the only MSR entry point invoked is
   `ioctl_read_msr`, and that the read set is a fixed allow-list of the MSR
   indices above (REQ-CPUEFF-05/06). A code-level guard (a single read-only MSR
   helper with a hard-coded allow-list) is the recommended enforcement.
2. **Gate only the MSR-derived fields on `strict` — do not regress temperature.**
   The bin is loaded `warn_only` today, which is acceptable for SMN temperature
   but not for counters that feed a reported efficiency number: a swapped bin
   could return fabricated counter values. The naive fix — promoting the shared
   `kPawnIoSpecAmdFamily17V1` to `strict` — **regresses a documented, by-design
   tolerance**: `warn_only` exists so "an existing install with a locally swapped
   AMDFamily17.bin keeps working on upgrade" (`pawnio_binary.h:26-32`), and a
   blanket `strict` would refuse *temperature* reads on a hash mismatch too, not
   just MSR reads. Recommended branch: keep the SMN/temperature path tolerant
   (`warn_only`) and gate **only** the MSR-derived energy/frequency fields on a
   strict hash check — on mismatch, blank the energy/frequency fields while
   temperature still reads. This preserves the upgrade tolerance and refuses to
   report efficiency numbers from an unverified bin.
3. **APERF/MPERF affinity can invalidate the ratio.** Without thread affinity,
   the two paired reads can run on different cores, and a ΔAPERF/ΔMPERF ratio whose
   reads straddle a core migration is meaningless — not merely coarse. The
   standard mitigation (LibreHardwareMonitor) is caller-side
   `SetThreadAffinityMask` around the paired reads, which needs **no module
   change** — but only if PawnIO runs the `rdmsr` on the calling thread. Whether
   PawnIO honors caller affinity (vs marshaling to its own context) is **unknown
   and decides whether v1 effective frequency is trustworthy at all**; it must be
   confirmed in live validation before any effective-frequency field is reported.
4. **32-bit energy wrap.** Energy deltas must be computed with 32-bit modular
   arithmetic and a guard that drops a window where the apparent delta is
   implausibly large (multiple wraps within one window), emitting blank rather
   than a false spike — consistent with FEAT-0002's no-false-zero rule.
5. **`#GP` on an unsupported MSR.** Reading a non-existent MSR faults; the driver
   returns an error, which must degrade the field to blank, not crash or zero.

## Recommended acquisition path (input to FEAT-0006 §9)

- **v1 (read-only, highest-reuse):** read `MSR_PKG_ENERGY_STAT` + `MSR_RAPL_PWR_UNIT`
  (package energy → Joules / average power) and `APERF`/`MPERF` (effective
  frequency, with the paired reads taken under `SetThreadAffinityMask` **pending
  confirmation that PawnIO honors caller affinity** — otherwise effective
  frequency is held back as unvalidated), through the existing `AMDFamily17.bin`
  via a new read-only MSR helper with a hard-coded allow-list, gating only the
  MSR-derived fields on a strict hash check (temperature stays `warn_only`).
- **Defer:** instructions-retired (needs PMC programming = MSR writes), per-core
  energy and per-core APERF/MPERF aggregation (Tier 3 / affinity), and Vcore +
  PPT/TDC/EDC (SMU mailbox, separate fragile path).
- **Then:** derive cycles-per-Joule and average power in the analyzer from the raw
  package-energy and APERF/MPERF deltas (work-per-Joule north-star), with busy
  time (FEAT-0002) as the time-normalization context.

## Follow-up status (updated 2026-06-07)

1. **Done:** `docs/cpu-work-energy-acquisition-decision-2026-06-07.md` commits
   the v1 path above (FEAT-0006 §9, gate 3).
2. **Done:** the counter-read safety review (FEAT-0006 §9,
   REQ-CPUEFF-05/06) is folded into that decision: read-only enforcement, the
   MSR allow-list, field-level strict hash gating, and `#GP`/wrap
   degrade-to-blank behavior.
3. **Done (2026-06-07):** the one-shot, read-only **live read validation** on the
   9950X3D, respecting `AGENTS.md` §Live Runtime Safety, checking: (a) **does
   PawnIO honor caller `SetThreadAffinityMask`** for the `rdmsr`; (b) **RAPL
   availability/encoding on Family 1Ah** (`0xC0010299`/`0xC001029B`);
   (c) APERF/MPERF effective frequency; (d) an unsupported MSR returns error →
   blank. Procedure: `docs/cpu-work-energy-live-validation-plan-2026-06-07.md`.
   **Outcome: PASS (energy-only).** RAPL energy works and is correctly encoded on
   Family 1Ah. The run also reported APERF/MPERF `#GP`, but that was a
   **probe-index error** (corrected 2026-06-09): it read the architectural
   `0xE7`/`0xE8`, which the module does not allow-list; the shipped bin **does**
   serve the AMD read-only aliases `0xC00000E7`/`0xC00000E8`, so APERF/MPERF and
   effective frequency are reachable (the §Per-signal reachability row stands).
   Results: `docs/cpu-work-energy-live-validation-results-2026-06-07.md`;
   resolution: `docs/cpu-cycle-counter-source-decision-2026-06-07.md`.
4. **After validation:** the maintainer decides the energy-only re-scope
   (cycle-counter work proxy and effective-frequency context withheld) and
   authorizes the build; then implement the energy path per the decision doc
   §Apply order.

## Sources

- [PawnIO.Modules (namazso) — upstream module project](https://github.com/namazso/PawnIO.Modules)
- [Linux turbostat AMD Fam 17h (Zen) RAPL MSR support (0xC0010299/29A/29B)](https://mailweb.openeuler.org/hyperkitty/list/kernel@openeuler.org/message/3KAC6DAF2CN3HVUVG2NBZJ6LHELVVX57/)
- [Kernel amd_energy driver (AMD RAPL MSR semantics)](https://www.kernel.org/doc/html/v5.9/hwmon/amd_energy.html)
- [Intel SDM Vol 3B — IA32_APERF (0xE8) / IA32_MPERF (0xE7) effective frequency](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3b-part-2-manual.pdf)
