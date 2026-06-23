# CPU Work/Energy Acquisition Decision - 2026-06-07

Status: **accepted** by the maintainer 2026-06-07. Design decision for
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`; settles
FEAT-0006 promotion **gate 3** (acquisition design record) and folds in the
**gate 6** counter-read safety review (§"Counter-read safety review" below).
FEAT-0006's promotion gates are met. This decision is
normative for the acquisition design. The one-shot read-only live MSR validation
has since run (2026-06-07): PASS (energy-only) — RAPL package energy works on
Family 1Ah. (The run also reported APERF/MPERF `#GP`, but that was a probe-index
error: the shipped bin serves the AMD read-only aliases `0xC00000E7`/`0xC00000E8`,
not the architectural `0xE7`/`0xE8` the probe read — corrected 2026-06-09.) Per
`AGENTS.md` §Feature Intake Gate the maintainer authorized an energy-only v1
build; the work numerator / effective frequency are deferred from that first cut
but reachable with the shipped bin, a near-term cycle-path addition (see
"Resolved live" below). FEAT-0004 is recommended
operational context, not a blocker.

**Companion to:**
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
`docs/cpu-work-energy-acquisition-verification-2026-06-04.md` (the feasibility
input this decision commits),
`docs/features/FEAT-0002-cpu-settings-evidence-logger.md` and its
`docs/cpu-settings-evidence-logger-decision-2026-06-04.md` (the time-layer
context),
`docs/features/FEAT-0004-hardware-access-health-signal.md`,
`docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/CONTROL_LOOP.md`, `AGENTS.md` §Live Runtime Safety / §Repo Boundary,
`src/hardware/amd_reader.cpp`, `src/hardware/pawnio_binary.h`.

## Scope: read-only, log-only

This decision covers **acquiring and logging** raw CPU work/energy counters as
first-party evidence. It is read-only and log-only:

- Acquisition reads MSRs only (`ioctl_read_msr`). The controller **must not**
  call `ioctl_write_msr` or write any CPU power/clock/voltage control
  (REQ-CPUEFF-05).
- The logged data **must not** feed any control path: no curve, blend, boost,
  cadence, write-gate, or feedforward input. A CPU power feedforward is a
  separate future feature and is explicitly out of scope here.
- The logger records raw counter deltas plus the resource-window identity and
  duration needed to interpret those deltas. Average power, effective frequency,
  and any efficiency ratio are derived later by the analyzer (REQ-CPUEFF-07).
- No fan write, start/stop, breaker reset, or cadence/channel change
  (`AGENTS.md` §Live Runtime Safety). This does not cross
  `docs/MEASUREMENT_GATE.md`.

## Problem

No CPU power or energy data is captured today; the only CPU signals logged are
temperature (`cpu_tctl_c`, per-CCD Tdie) and whole-system busy *time*
(FEAT-0002, `system_cpu_busy_pct` from `GetSystemTimes`). Busy time measures
*how long* cores were active, not *how much energy* was spent, so raw cooling
performance — heat dissipated per unit airflow — cannot be read from the logs.

The 2026-06-04 verification established that the data is reachable read-only
through the already-shipped transport: `AMDFamily17.bin` exposes
`ioctl_read_msr`, and `ExecutePawnIo` (`amd_reader.cpp:210-253`) is a generic
function-name dispatcher, so an MSR read is the same call shape as the existing
`ioctl_read_smn` temperature read with a different function name. No new kernel
module and no transport-layer change is required. This decision commits the
acquisition path and the safeguards around it.

## Options considered

### Energy source

**Option A — AMD RAPL package energy MSR (chosen).**
`MSR_PKG_ENERGY_STAT 0xC001029B` (socket-level 32-bit energy accumulator, any
core returns the socket total, updated ~every 1 ms) scaled by
`MSR_RAPL_PWR_UNIT 0xC0010299` (energy unit `1/2^ESU` J; ESU default `0b10000`
→ 15.26 µJ/unit). Read-only, no PMC programming, no thread affinity (package
scope). Reachable through the shipped bin (verification doc §Per-signal feasibility, package-energy row).

- Caveat carried as a quarantine item: RAPL availability/encoding on this part
  (Ryzen 9 9950X3D, Family 1Ah / Zen 5, model `0x44`) is documented only for
  Family 17h+; it is not confirmed for Family 1Ah and is validated live before
  the data is trusted (verification doc §Per-signal feasibility, package-energy row).

**Option B — SMU power-reporting table (PPT) over SMN mailbox.** Rejected for
v1: the SMU mailbox layout is firmware-version-specific and fragile, and it
adds a write-then-read mailbox sequence on the SMN/PCI path that the energy MSR
avoids (verification doc §Per-signal feasibility, PPT/TDC/EDC row; FEAT-0006 §11). Deferred with Vcore/PPT/TDC/EDC.

**Option C — SVI2/SVI3 voltage × current (rejected).** AMD exposes per-domain
core (VDDCR_CPU) and SoC (VDDCR_SOC) voltage and current through the SMU SVI
telemetry; power is their product — this is HWiNFO's "SVI2/SVI3 TFN" power
sensor (distinct from its SMU-table "CPU Package Power (PPT)" sensor). Rejected
for v1 for three reasons:

- It rides the same SMU-mailbox / SMN telemetry path as Option B, which is
  firmware-version-specific and fragile (verification doc §Per-signal feasibility, Vcore/VID and PPT/TDC/EDC rows).
- The SVI **current** reading is meaningless without a **per-motherboard**
  telemetry scaling factor (the VRM telemetry-current reference baked into board
  firmware). That factor is not discoverable first-party from the CPU, and
  sourcing it from an external per-board database would break `AGENTS.md` §Repo
  Boundary — so an uncalibrated `V × I` is off by a multiplicative error (the
  known "SVI power is wrong on board X" failure).
- It covers only the core + SoC rails, not the package total.

RAPL package energy (Option A) yields package power directly, already calibrated
to Joules by the on-chip energy unit `0xC0010299`, with no voltage, current, or
per-board factor — so `V × I` is the more fragile path for a worse-scoped
number. CPU voltage as Tier-2 *context* (not as the power signal) stays a
deferred SMU-mailbox best-effort item, blank when the firmware layout is
unknown (verification doc §Per-signal feasibility, Vcore/VID row).

**Option D — none / status quo.** Rejected: leaves raw cooling performance
unmeasurable (the Problem).

### Work proxy

**Option A — APERF/MPERF delivered cycles + effective frequency (chosen for the
work/frequency signal).** `IA32_APERF 0xE8` / `IA32_MPERF 0xE7` are
architectural, read-only, free-running. Effective frequency derives from the
`ΔAPERF/ΔMPERF` ratio. These are per-logical-processor and need thread affinity
around the paired reads, which is a disturbance and validity risk handled below.

**Option B — instructions retired (PMC `INST_RETIRED`).** Rejected for v1:
programming a PMC needs `ioctl_write_msr`, which collides with read-only
(REQ-CPUEFF-05). Deferred; APERF delivered cycles is the read-only v1 work
proxy (FEAT-0006 §11 default).

### Bin hash-verification mode for MSR-derived fields

**Option A — strict hash gate on the MSR-derived fields only (chosen).** Keep
the SMN/temperature path `warn_only` (its current mode,
`pawnio_binary.cpp:140`) and gate **only** the MSR-derived energy/frequency
fields on a strict hash check: on a hash mismatch, blank the energy/frequency
fields while temperature still reads. This is a field-level gate enforced in the
new MSR helper, not a new `PawnIoVerification` mode (see the Counter-read safety
review for the enforcement path and its loader prerequisite).

**Option B — flip the shared spec to strict.** Rejected: a blanket strict mode
regresses a by-design tolerance — `warn_only` exists so an existing install
with a locally swapped `AMDFamily17.bin` keeps reading *temperature* on upgrade
(`pawnio_binary.h:24-32`); blanket strict would refuse temperature reads on a
hash mismatch too (verification doc risk #2).

## Decision

Adopt **v1 = AMD RAPL package energy (`0xC001029B` + unit `0xC0010299`) plus
APERF/MPERF (`0xE8`/`0xE7`)**, read through the existing `AMDFamily17.bin` via a
new read-only MSR helper with a hard-coded allow-list, with the **MSR-derived
fields gated strict** while temperature stays `warn_only`, **shipped disabled by
default**, and the data held in **quarantine** until the Evaluation criteria
pass.

Deferred (not in v1): `INST_RETIRED` (needs PMC writes), per-core energy
(`0xC001029A`, Tier 3), and Vcore + PPT/TDC/EDC (SMU mailbox).

Effective frequency is included **only if** live validation confirms PawnIO
honors caller thread affinity for `rdmsr` (see Evaluation §4); otherwise v1
ships energy-only and effective frequency stays withheld.

This means FEAT-0006's Tier 2 context is intentionally split for v1:
already-logged CPU temperature remains available with the work/energy window,
APERF/MPERF inputs are recorded only after affinity validation, and Vcore/VID
plus PPT/TDC/EDC throttle context remain unavailable/deferred rather than
guessed or represented as measured values.

New control-loop fields (additive, nullable; final names settled at
implementation per FEAT-0006 §7), proposed:

- `cpu_power_sample_id` — monotonic resource-window sample id. It increments
  only when a new CPU work/energy resource-window sample is taken. Repeated
  250 ms control-loop rows may mirror the latest id, but the analyzer counts a
  sample id only once.
- `cpu_power_window_ms` — elapsed window used for the deltas. Blank when there
  is no previous valid sample, a skipped window, or an invalid/over-long window.
- `cpu_pkg_energy_delta_uj` — raw package-energy delta over the resource window,
  in energy units converted to microjoules (32-bit modular subtraction applied).
- `cpu_aperf_delta`, `cpu_mperf_delta` — delivered/reference cycle deltas
  (present only when effective frequency is validated).
- `cpu_pkg_energy_acquisition` — package-energy provenance marker, one of
  `disabled` / `unavailable` / `quarantine` / `validated` (see Quarantine).
- `cpu_cycles_acquisition` — APERF/MPERF provenance marker, one of `disabled` /
  `unavailable` / `quarantine` / `validated`. This is separate from package
  energy because RAPL can validate while APERF/MPERF remains unavailable or
  affinity-untrusted.
- Optional nullable reason fields may be added at implementation
  (`cpu_pkg_energy_blank_reason`, `cpu_cycles_blank_reason`) if they are useful
  for analyzer diagnostics; they must never substitute zero for missing data.

Average package power (watts) and effective frequency are **derived in the
analyzer**, not logged.

## Counter-read safety review (FEAT-0006 gate 6 / REQ-CPUEFF-05, REQ-CPUEFF-06)

This section is the counter-read safety review the verification doc allows to be
folded into the decision (verification doc §Follow-up status).

- **Read-only allow-list.** All MSR access goes through one helper that accepts
  only a fixed allow-list of indices: `0xC001029B`, `0xC0010299`, and (when
  effective frequency is enabled) `0xE7`, `0xE8`. The helper **must** reject any
  other index and **must never** call `ioctl_write_msr`. A unit test enforces
  both (Verification).
- **No CPU actuation.** No PMC programming, no SMU control mailbox, no
  power/clock/voltage write. Reading counters is in scope; actuating CPU state
  is not (REQ-CPUEFF-05).
- **Strict gate, MSR-derived only.** The bin keeps loading via
  `kPawnIoSpecAmdFamily17V1` in `warn_only` (so temperature is unaffected); the
  new MSR helper consults the load's hash-verification result and, on a recorded
  mismatch, blanks the MSR-derived energy/frequency fields while temperature
  still reads. No new `PawnIoVerification` enum value is introduced — the
  field-level gate lives in the helper, not in the bin loader.
  **Implementation prerequisite:** the loader must surface a *structured*
  hash-mismatch flag for the helper to consult; today a `warn_only` mismatch is
  only the `init_warning()` string (`pawnio_binary.cpp:287-293`), and
  string-matching that text is too fragile to gate a reported number on.
- **`#GP` degrades to blank.** Reading an absent/unsupported MSR faults; the
  driver returns an error, which **must** blank that field — never crash, never
  emit a zero (no-false-zero, FEAT-0002 §5).
- **32-bit wrap is handled, not assumed away.** `0xC001029B` is a 32-bit
  accumulator; deltas **must** use 32-bit modular subtraction
  (`(cur - prev) & 0xFFFFFFFF`). See Derivation for why a single window cannot
  multi-wrap, and the two guards (power ceiling and Δt bound) that blank a
  window that cannot be trusted.

## Derivation (normative for implementation)

Over a resource window from sample `p` to sample `c` of duration `Δt` seconds:

```
# Package energy (units, 32-bit modular)
units_delta = (c.pkg_energy_raw - p.pkg_energy_raw) & 0xFFFFFFFF
uj_per_unit = 1e6 / 2^ESU            # ESU from 0xC0010299; default ESU=16 -> 15.26 uj
cpu_pkg_energy_delta_uj = units_delta * uj_per_unit

# Derived later, in the analyzer (NOT logged):
avg_pkg_power_w = (cpu_pkg_energy_delta_uj / 1e6) / Δt

# Effective frequency (only when APERF/MPERF validated):
effective_freq = (Δaperf / Δmperf) * reference_freq    # architectural ratio
```

- `cpu_power_sample_id` is the analyzer de-duplication key. The control-loop CSV
  can contain several 250 ms rows with the same latest resource-window sample,
  matching the existing process/system CPU resource fields. Analyzer/reporting
  must aggregate only distinct nonblank sample ids and must ignore repeated rows
  with the same id for energy, power, and cycles-per-Joule math.
- **A single normal window cannot multi-wrap.** At 15.26 µJ/unit, `2^32` units
  ≈ 65.5 kJ. The resource window is ~1 s (the existing FEAT-0002 resource
  window). Wrapping in ~1 s would require > ~65 kW, which is physically
  impossible for this socket, so one modular subtraction is exact within a
  normal window. The counter wraps about every 5–6 min at 200 W — relevant only
  across windows, not within one (verification doc §Per-signal feasibility, package-energy row; §Key risks and required safeguards). These
  constants assume the default ESU = 16 (15.26 µJ/unit) and are recomputed from
  the runtime-read `0xC0010299`, so they are provisional until the live ESU read
  confirms the encoding on Family 1Ah (Evaluation §5).
- **Implausibility guard (two independent branches).** Leave the field blank and
  emit no value when either branch trips:
  - *High-power ceiling:* the modular `units_delta` implies an average power
    above a hard socket ceiling (a fixed constant well above the part's
    configured PPT) — catches a short-window over-read (apparent power *above*
    the ceiling).
  - *Δt upper bound:* the window duration is missing/non-positive **or exceeds
    `k × poll window` (k ≈ 2–3, i.e. a few seconds)** — catches an over-long
    window (a missed sample, a skipped window per Disturbance mitigation §6, or
    OS sleep/resume). This branch is load-bearing: the modular subtraction is
    exact only within a single sub-wrap (~1 s) window, and over a long gap
    multiple wraps alias to a plausible *low* power that the high-power branch
    cannot catch — at a true 200 W, a 400 s gap differences to 14,464 J ≈ 36 W
    (a false low), and for any Δt ≳ 327 s the whole 65,536 J range ÷ Δt lands
    below 200 W. The Δt cap is the only branch that enforces the "a single normal
    window cannot multi-wrap" invariant.
- **APERF/MPERF guard.** The ratio is meaningful only when both reads were taken
  on the same core under held affinity (Disturbance mitigation §affinity). When
  affinity is not held or not honored, blank the cycle deltas rather than log a
  cross-core ratio.
- Guard like the existing process path: require a previous valid sample, a
  positive window, and a successful read; on any failure leave the dependent
  fields blank/null.

## Disturbance mitigation (keep the live control loop undisturbed)

The new reads run inside a live 250 ms control loop. The path is shaped so a
steady-state tick is unchanged:

1. **Off by default.** The MSR read path ships disabled
   (`cpu_pkg_energy_acquisition = disabled`,
   `cpu_cycles_acquisition = disabled`); enabling it is an explicit opt-in. The
   default live controller behaves exactly as today until the path is trusted.
2. **Resource-window cadence, not tick cadence.** The MSR reads ride the
   existing ~1 s resource window that already samples `GetSystemTimes` /
   `GetProcessTimes`, not the 250 ms control tick. Steady-state ticks (~4/s) add
   zero MSR work; only ~1 window/s does 1–3 extra `DeviceIoControl` calls.
3. **Outside the PCI mutex.** SMN temperature reads hold `Global\Access_PCI`
   once per `Sample()` because SMN goes through PCI config space
   (`amd_reader.cpp:567-580`). `rdmsr` does not use PCI config space, so the MSR
   reads **must** run outside that critical section: they do not extend the
   mutex hold time and do not contend with the temperature path or external PCI
   users. (Whether PawnIO serializes MSR reads internally is unconfirmed and is
   a live-validation item.)
4. **Fault isolation.** The MSR step runs after the temperature `Sample()`
   returns and is fully guarded; any error blanks only the MSR-derived fields.
   No new failure can abort or stall a tick, and temperature is never gated on
   MSR success.
5. **Affinity without control-thread churn.** APERF/MPERF need paired reads
   under `SetThreadAffinityMask`. To avoid repinning the control thread
   (scheduling jitter), v1 either (a) ships energy-only (energy is package-scope,
   no affinity) until affinity-honoring is confirmed, or (b) takes the paired
   read on a dedicated short-lived sampler thread — never repinning the control
   thread.
6. **Budget skip.** If a window's MSR read would risk a tick overrun, skip it
   and blank that window rather than overrun; blanks are already tolerated.

The no-disturbance claim is not assumed — it is measured as a quarantine-exit
gate (Evaluation §6).

## Quarantine (collect, but distrust until validated)

New telemetry from an unexercised, hardware-unconfirmed path is collected but
**not trusted** until it proves itself:

- **Self-labeling provenance.** While enabled but unproven, every row and the
  run manifest carry per-signal quarantine markers
  (`cpu_pkg_energy_acquisition = quarantine`,
  `cpu_cycles_acquisition = quarantine` when cycles are enabled). Any archive
  captured in this state is self-labeling, so the analyzer and operator do not
  treat the numbers as validated.
- **No trusted derivation during quarantine.** The analyzer surfaces quarantined
  energy/power as experimental only; it does not enter any reported efficiency
  verdict or tuning conclusion (extends REQ-CPUEFF-07).
- **No control use, ever in this scope.** Read-only, log-only — quarantine
  status does not gate any control path because there is none (Scope).
- **Exit is an explicit maintainer decision over independent captures.**
  Quarantine ends only when the Evaluation criteria hold across **at least 3
  independent capture sessions** -- not a single window -- so the bar is
  repeatability, not one plausible run. The earlier fixed **>= 7 day** span was
  removed on 2026-06-14 because it was an unsupported policy margin, not a
  measured requirement. Reliability is demonstrated by separate capture sessions,
  worker restart cycles, idle/load/cooldown phase coverage, and the criteria
  below. Any future calendar-delay gate needs an explicit rationale and decision
  record. The decision is recorded in a follow-up validation note; the marker
  then flips to `cpu_pkg_energy_acquisition = validated`.
  `cpu_cycles_acquisition` flips only if the separate APERF/MPERF affinity and
  plausibility gates also pass. Promotion is never automatic.
- If RAPL proves unavailable or implausible on this part, the path stays
  quarantined indefinitely (it failed on this hardware) and is not promoted.

## Evaluation (quarantine-exit criteria)

The data is promoted from `quarantine` to `validated` only when all of the
following hold over an evidence window that spans idle, a sustained CPU load
(e.g. y-cruncher, long enough to cross at least one 32-bit energy wrap ≈ 5–6 min
at ~200 W), and cooldown. Use the in-tree analyzer
(`svg-mb-control analyze ingest`/`report`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`) over the captured CSV.

1. **Counter continuity across a wrap.** Across ≥ 1 full 32-bit wrap, the
   modular-differenced energy stays continuous — no power spike, no blank-storm;
   the implausibility guard does not fire within a normal (~1 s) window.
2. **Plausible range and load tracking.** Derived average package power is
   non-negative, below the socket ceiling, near its floor at idle, and rises and
   falls with the load on/off transitions. It is rejected if it is pinned
   constant regardless of load, negative, or above the physical ceiling.
3. **External one-shot cross-check (independent source).** Over a steady
   controlled window, the energy-derived average watts agree within **±15%** with
   an external reference taken from a path **independent of the RAPL energy
   MSR** — SMU/PPT telemetry (Ryzen Master is SMU/PPT-sourced and qualifies; for
   HWiNFO use the "CPU Package Power (SMU)" sensor, not a RAPL-MSR-derived
   sensor). A RAPL-derived reference reads `0xC001029B` with the same ESU
   assumption, so ±15% agreement would be near-tautological and could not detect
   a wrong-encoding error. *Fallback:* if only a RAPL-derived external value is
   available, §3 then validates Δt/window handling but **not** the ESU encoding,
   and encoding correctness must be argued separately via idle-floor +
   load-tracking plausibility and a documented ESU source. This is a one-shot
   **manual** comparison recorded in the exit note — **not** a runtime
   dependency (`AGENTS.md` §Repo Boundary).
4. **Effective-frequency validity (only if APERF/MPERF is included).**
   `ΔAPERF/ΔMPERF` yields a plausible effective clock (between idle and rated
   boost) and is stable while affinity is held. If PawnIO does not honor caller
   affinity, the ratio shows impossible values; effective frequency then stays
   quarantined while energy may still pass independently.
5. **Fault behavior.** An unsupported/absent MSR (including RAPL absent on
   Family 1Ah) blanks cleanly — no crash, no false zero. If RAPL is unavailable
   on this part, energy stays quarantined (fail on this hardware).
6. **No-disturbance gate.** Against a named pre-change baseline session,
   `loop_slip_ms` p95 increase ≤ X ms and max ≤ Y ms, with X/Y set from the
   baseline's observed variance (e.g. ≤ 1 baseline standard deviation), **and**
   zero new `loop_overrun` rows, no increase in SMN/temperature read failures,
   and no increase in steady-state `control_loop.authority_reasserted` events
   after excluding startup/baseline-capture rows — all read from the same
   control-loop CSV/event log. (X/Y and the steady-state exclusion window are
   fixed against the chosen baseline at validation time, not invented here.) If
   timing regresses, fix the read placement/cadence before promotion.

## Apply order (normative for implementation)

1. **Helper.** Add the read-only MSR helper (allow-list; `#GP`→blank; field-level
   strict gate that consumes the loader's structured hash-mismatch verdict and
   blanks MSR-derived fields on mismatch — see Counter-read safety review) over
   `ExecutePawnIo` with `ioctl_read_msr`; it never calls `ioctl_write_msr`.
2. **Sample.** Read `0xC0010299` once at init and cache the energy unit (ESU is
   constant for the part). On the existing ~1 s resource window (next to the
   `GetSystemTimes` read), read `0xC001029B` for package energy — and, when
   effective frequency is enabled, `0xE8`/`0xE7` — outside the
   `Global\Access_PCI` critical section.
3. **Derive deltas.** Apply 32-bit modular subtraction and the implausibility
   guard; assign a new `cpu_power_sample_id` only for a new resource-window
   sample, carry `cpu_power_window_ms`, `cpu_pkg_energy_delta_uj` (and cycle
   deltas if validated) onto the tick state, and blank dependent fields on any
   failure.
4. **Emit.** Add the additive nullable columns and per-signal acquisition
   markers to `BuildControlLoopCsvHeader`/`Row`, grouped with the FEAT-0002
   system-CPU block; default markers to `disabled`.
5. **Analyzer.** Derive average watts (`ΔJ/Δt`) and effective frequency in the
   analyzer over distinct `cpu_power_sample_id` values; surface quarantined
   values as experimental only.

## Consequences

- New columns are additive and name-bound, so archives without them still parse
  (matches the FEAT-0002 decision's backward-compatibility rule).
- No control-computation identity change; `docs/CONTROL_PIPELINE_MATH.md` is
  unaffected (read-only, log-only evidence).
- `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` gain the
  new columns and the per-signal quarantine markers at implementation
  (`AGENTS.md` §Change Checklist).
- Maintainer acceptance on 2026-06-07 updated FEAT-0006 §9 and §13: the
  acquisition-decision row points to this doc, the counter-read safety review is
  folded into this doc, and the `REQ-CPUEFF-*` rows are mirrored in
  `docs/TRACEABILITY.md`. The accepted rule is "bin stays `warn_only`;
  MSR-derived fields are gated strict at field level."

## Verification

- `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`:
  - C++ test of 32-bit modular energy-delta math, including an exactly-one-wrap
    case, a missed-sample / over-long-window → blank case, and a long-window
    multi-wrap-aliasing-to-low-power case (e.g. Δt = 400 s at true 200 W →
    14,464 J modular delta → apparent 36 W) asserting the field is blanked;
  - energy-unit decode from `0xC0010299` (ESU → µJ/unit);
  - effective-frequency ratio from synthetic APERF/MPERF deltas;
  - sample-id/window handling: repeated 250 ms CSV rows with one
    `cpu_power_sample_id` do not double-count energy in analyzer output;
  - per-signal provenance: package energy can be `validated` while cycles remain
    `disabled`, `unavailable`, or `quarantine`;
  - allow-list guard test: the helper rejects any non-allow-listed MSR index and
    has no `ioctl_write_msr` call site;
  - `BuildControlLoopCsvHeader`/`Row` stay aligned and contain the new columns;
  - no-false-zero on a backward or implausible counter.
- Backward-compatibility: analyzer/ingest parse of an archive missing the new
  columns still succeeds (name-bound `GetField`).
- Code review vs this decision and `AGENTS.md` §Live Runtime Safety / §Repo
  Boundary: reads only, strict gate on MSR-derived fields only, default-off,
  per-signal quarantine markers present, no control consumption.
- Manual (read-only, post-decision/pre-implementation): the one-shot live
  validation from the verification doc follow-up status (RAPL on Family 1Ah;
  PawnIO affinity honoring;
  `#GP`→blank), feeding the Evaluation. Its procedure is
  `docs/archive/implemented-plans/cpu-work-energy-live-validation-plan-2026-06-07.md`.

## What this does not authorize / open items

- **Implementation permission (energy-only).** FEAT-0006's promotion gates are
  met and the one-shot read-only live MSR validation passed 2026-06-07
  (energy-only); the maintainer authorized an energy-only v1 build. The
  work-numerator / effective-frequency slice is deferred from that first cut but
  reachable with the shipped bin (AMD RO aliases `0xC00000E7`/`0xC00000E8`;
  corrected 2026-06-09) — a near-term cycle-path addition, not a new source.
  FEAT-0004 is recommended operational context, not a blocker.
- **Deferred signals.** `INST_RETIRED` (PMC writes), per-core energy (Tier 3),
  Vcore + PPT/TDC/EDC (SMU mailbox) — out of v1. They remain target context for
  a later source decision, not v1 pass/fail fields. Until those fields exist,
  analyzer/reporting must make clear that efficiency evidence lacks those
  confounder checks.
- **No control/feedforward use** of this data — a separate future feature.
- **Resolved live (2026-06-07; see
  `docs/cpu-work-energy-live-validation-results-2026-06-07.md`):** RAPL
  availability/encoding on Family 1Ah is **confirmed working** (ESU=16 →
  15.26 µJ/unit; power tracks load). The 2026-06-07 run also reported APERF/MPERF
  `#GP`, but that was a **probe-index error** (corrected 2026-06-09): it read the
  architectural `0xE7`/`0xE8`, which the module does not allow-list; the shipped
  bin **does** serve the AMD read-only aliases `0xC00000E7`/`0xC00000E8`. So the
  work/effective-frequency path needs **no new module** — it is a near-term
  cycle-path addition gated on a corrected per-core-pinned live read (the PawnIO
  affinity question is live again, not moot). As **implemented**, v1 is still
  energy-only. See `docs/cpu-cycle-counter-source-decision-2026-06-07.md`
  (resolved).
- **Still open:** the exact socket-ceiling constant for the implausibility guard
  (the validation plan proposes 400 W; finalize at implementation).
