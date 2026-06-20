# Idle CPU temperature trend and data-enrichment evaluation (2026-06-20)

## Scope

Two questions:

1. Have recent CPU temperatures (idle and low load) become lower than the
   preceding weeks, and if so, why?
2. What data enrichment would let us attribute temperature changes to a cause
   instead of guessing?

All numbers below are read-only aggregates over data already on disk. No control
behavior was changed. Claims labeled **inference** are not directly instrumented.

## Data sources and spans

| Source | Span | What it carries | Used for |
|---|---|---|---|
| `release/runtime/svg_mb_control.db` `tick_samples` (2.11M rows) | 2026-06-11 14:35 .. 2026-06-18 18:38 | per-tick `cpu_tctl_c`, `cpu_max_c`, GPU temps; power only from 06-18 | high-resolution idle-floor + GPU ambient anchor |
| same DB `events` (754k rows) | 2026-05-21 .. 2026-06-18 | `control_loop.write_applied` rows carry `observed_temp_c` per channel | the only CPU-temp signal reaching back ~4 weeks |
| `tick_fan_samples` / `tick_channel_samples` (14.8M each) | 06-11 .. 06-18 | fan rpm/duty, channel setpoint/source | controller-output verification |
| `release/runtime/experiments/cpu-temp-comparison/ledger.csv` | 2026-06-09 .. 2026-06-20 | idle/low/high `tctl` percentiles, busy band, `ambient_c` (manual), `git_hash`, `config_sha256` | busy-gated idle trend with provenance |

The structured per-tick store is ~9 days, not months. The `events` table extends
a usable CPU idle-floor proxy back to **2026-05-21** (~4 weeks). `ambient_c` is a
hand-typed constant (`21`, `22` on the 06-09 shakedown), not a measurement.

## Finding 1 — idle temps are recently at the cool end of a noisy band

Idle Tctl p50, busy-gated 0-10% (ledger), ambient logged constant 21 C:

| Day | 06-09 | 06-10 | 06-11 | 06-12 | 06-13 | 06-14 | 06-15 | 06-16 | 06-17 | 06-18 | 06-19 | 06-20 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Idle Tctl p50 (C) | 58.5 | 56.2 | 57.6 | 55.0 | 53.7 | **59.4** | 55.1 | 52.8 | **47.3** | 49.8 | 48.4 | 50.4 |

Independent cross-check from `tick_samples` (CPU idle floor = `cpu_tctl_c` p05;
GPU core idle floor = `gpu_core_c` p05, used below as an ambient anchor):

| Day | 06-11 | 06-12 | 06-13 | 06-14 | 06-15 | 06-16 | 06-17 | 06-18 |
|---|---|---|---|---|---|---|---|---|
| CPU tctl p05 (C) | 51.5 | 51.5 | 52.6 | **57.1** | 49.8 | 49.6 | **45.6** | 52.2 |
| GPU core p05 (C) | 28.9 | 29.3 | 26.9 | 29.4 | 25.2 | 29.9 | 29.4 | 27.0 |

Both methods agree on shape (06-14 hot bump, 06-17 low). Low band fell ~66 -> ~59 C;
high band ~84 -> ~77 C (high band is sparse and noisier).

**The month view changes the framing.** Per-day p05 of `events.observed_temp_c`
for CPU-driven channels (2/3, validated against `cpu_tctl_c` on the overlap:
corr 0.90, +1.3 C bias) over 05-21..06-18 spans **42-57 C with 13 day-to-day sign
reversals**. There is no monotonic decline (Spearman vs day-index rho=+0.289,
permutation p=0.13; the sign is mildly *positive*). Four days in late May / early
June (05-27 ~44.7, and 06-01/06-02/06-05 ~42-44) were **colder than the recent
"low" of 06-17 (47.9)**. There is a validated sharp **upward step on 06-09**
(pre-09 mean floor 48.7 C vs 06-09-onward 53.2 C; +4.6 C, Welch t=-4.05; single-day
+7.2 C; hourly resolution confirms an abrupt within-day onset).

So: recent idle temps **are** at the cool end, but as the low end of a recurring
~48-58 C band, not a steady improvement. Your perception is correct for
recent-vs-the-preceding-week; it is not a permanent step down.

## Finding 2 — what is NOT causing the swing (each independently verified)

Each claim below was re-derived by an independent adversarial pass tasked with
refuting it; all four came back **confirmed**.

- **Room ambient drift — ruled out.** The GPU core idle floor is an independent
  ambient anchor (its core-to-memory-junction offset is stable at 11.1-12.1 C
  every day, and the two GPU sensors correlate r=+0.97 day-to-day, so the GPU's
  physical relationship to intake air never changed). CPU idle floor vs GPU idle
  floor: Pearson r=-0.005, OLS R^2=0.000 — the ambient anchor explains **zero**
  CPU-floor variance. The relationship is in fact anti-correlated by rank: the
  two coldest CPU days (06-17, 06-16) are among the warmest GPU days.
- **Controller fans / pump — ruled out.** Idle (cpu_tctl<55) per-fan duty and rpm
  are flat across 06-11..06-18 (fan1 duty 22.0-22.4%, fan5 20.0-20.4%, AIO pump
  index 6 constant 2749-2760 rpm / 80.8%). Channel setpoints invariant
  (ch1=22.00, ch5=20.00 every day). On the cold day (06-17) the curves commanded
  *marginally lower* duty than the warm day (06-13) at matched thermal state — the
  opposite of a cooling-driven cause. The `low_band_stage`/`cpu_low_soak` boosts
  are ~0.000% at idle.
- **Low-band soak feature — ruled out.** `control_loop.low_band_stage_activated`
  fires single digits per day with no clustering at the drop.
- **Thermal-paste cure / fresh-build settling — ruled out.** A curing interface
  produces a monotonic decline then plateau. The month data shows oscillation, a
  06-09 step *up*, and colder floors weeks *earlier* — a cure cannot produce that.
  The 06-09 ledger entry is the measurement-harness shakedown, not a hardware
  build; the system was logging temperatures back to 05-21.
- **Sampling / diurnal artifact — ruled out.** The recent cool state is real, not
  a selection effect: on 06-17, 21.6% of all-day samples are below 50 C (13.5%
  below 48 C) versus literally 0 samples below 50 C all day on 06-13 and 06-14;
  06-17 is 7-15 C cooler than 06-14 at *every* matched hour; background-activity
  proxies (`process_cpu_pct`, `loop_work_duration_ms`) are flat across days.

## Finding 3 — the most likely cause (inference, given the instrumentation gap)

With ambient flat, controller output flat, and the interface not curing, the idle
floor is moving on the **heat-generation / recent-usage** side:

- The 06-09 -> 06-11 elevation coincides with a documented period of release
  rebuilds, harness CPU-test activity (the ledger 06-09 note states load "is not
  representative normal use"), and other project activity. The recent quieter
  period (06-16 -> 06-20) returns the floor to the cool end.
- Even a busy-gated "idle" reading carries **coolant thermal soak**: after a
  build- or test-heavy stretch the AIO loop stays warm, so idle Tctl measured
  shortly after load is elevated. The busy gate cannot remove this inertia. A
  genuinely quiet day reaches a genuinely cold loop.

This is consistent with all the evidence but **cannot be confirmed from the data**,
because the variables that would prove it — idle CPU package power, a workload
context label, and any operator/BIOS/power-plan action — were not recorded during
the window the floor moved (CPU power logging began only on 06-18; `ambient_c` is a
typed-in constant; operator actions are logged nowhere). That gap is exactly the
subject of the next section.

## Post-analysis disclosure (2026-06-20)

Findings 1-3 above were produced **blind**: the operator deliberately withheld a
hardware change so it could not bias the analysis. After the analysis was written,
the operator disclosed a **new/updated BIOS, a -25 all-core Curve Optimizer
undervolt, and retuned ("more sane") CPU settings**.

This **confirms the blind Finding-3 attribution**: the analysis independently ruled
out ambient, cooling, and physical-interface causes and located the cause on the
heat-generation / CPU-power side, naming "platform/BIOS/power" as a candidate. A
-25 CO undervolt plus tamed boost/PBO/SoC settings reduce Vcore/VID and transient
single-core boost spikes, lowering both load and idle temperatures — the mechanism
the blind analysis pointed to.

Two honest caveats, to avoid confirmation bias now that the cause is known:

1. **Weighting correction.** The blind write-up ranked recent-usage variance plus
   coolant soak as the *primary* driver, with BIOS/power secondary. The disclosure
   indicates the platform change deserved more weight. The *direction* (heat-
   generation side) was correct; the within-branch ranking was not. Usage variance
   is still real (the verified +4.6 C step on 06-09 is genuine build/test churn),
   so the recent cool bias is the undervolt operating on top of usage noise.
2. **The data cannot quantify or time the -25.** The per-capture idle floor
   oscillates 43-58 C across 06-16..06-20 (06-20 morning was still 50-55 C) with no
   clean onset step attributable to a BIOS-apply moment. A CO undervolt shows
   cleanest at load, the idle effect is modest and swamped by boost/usage swings,
   CPU power logging began only on 06-18 (no pre-change power baseline), and the
   change was not marked. Therefore this document does **not** claim the data proves
   the -25 caused the drop; it claims the -25 is consistent with and explains the
   blind heat-generation conclusion. Quantifying the benefit requires a controlled
   before/after at matched load with power logged and the change recorded as a
   marker (R2 + R3 below) — not a retrospective read of idle noise.

Post-change baseline (live session 2026-06-20T16:25, git `505c2495`, ambient
assumed 21 C, via `analyze_cpu_temp_power.py`): idle band (0-60 W) p50 41.4 W /
Tctl 48.0 C / theta 0.650 C/W; moderate band (60-100 W) p50 68.6 W / Tctl 61.5 C /
theta 0.586 C/W. No sustained high-load (>100 W) windows in this session, so the
load regime where the undervolt is most visible is not yet captured post-change.

The ledger label `stock-preoc` is now inaccurate (the box is no longer stock); it
should be relabeled (for example `bios-co-minus25`) from the change forward so the
trend self-documents the new regime.

## Enrichment roadmap — getting the context of use

Each item is grounded in the actual logging/ingest/analyzer code, is additive
(no removal of existing behavior), and is a recommendation, not implemented
behavior. Effort is S/M/L. They compose: 1 fixes the metric, 2+3 add context,
4 unifies.

### Principle: never assume operator settings; detect regime changes from data

Operator CPU/BIOS settings (Curve Optimizer offset, PBO/power limits, EXPO,
voltages, "sane" tunes) change over time and **must not be assumed or hard-coded**
in analysis tooling or docs as known constants:

- Settings change without notice, so any assumed value goes stale and biases
  cross-session comparison.
- The settings matter only for analysis interpretation, not for control — the
  controller does not need them.
- The operator will not hand-update an annotation each time a setting changes, so
  analysis **must not depend on manual operator annotation** for settings changes.

Therefore the analysis layer treats operator settings as a latent, unknown
variable and **detects regime changes automatically from telemetry** — a CPU
config fingerprint (see "CPU-side telemetry" below) that steps when settings
change, so the analyzer self-segments history into regimes instead of trusting a
hand-typed label (the `stock-preoc` label problem). This supersedes the
operator-marker emphasis below for settings changes: keep manual markers only for
genuinely un-derivable physical events (hardware reseat, room move), not for
BIOS/voltage tweaks, which are inferred from data.

### R1 — Computed `ambient_proxy_c` from the GPU idle floor (effort M, do first)

**Why:** `scripts/analyze_cpu_temp_power.py` derives `temp_rise_c` (line 353) and
`theta_c_per_w` (lines 355-356) — its only power-normalized cooling metrics — by
subtracting the hand-typed constant ambient. At 50 W a 3 C ambient error is a 6%
theta error, so cross-session cooling comparisons are currently anchored to
fiction. This analysis already proved the GPU idle floor is a flat, load-independent
ambient anchor.

**Mechanism:** per CPU package-energy window, `ambient_proxy_c = p05(gpu_core_c)`
over a rolling GPU-idle window (gate on `gpu_power_mw < ~60000` where present, else
`gpu_core_c` below its session p20), minus a one-time calibration offset (0 until a
known-room-temp capture sets it; even at 0 it removes day-to-day drift as a
*relative* anchor). Emit `ambient_proxy_c`, `ambient_proxy_source`
(`gpu_core_p05` | `gpu_memjn_p05` | `unavailable`), and `idle_sample_count`; fall
to `None` (never a false constant) when idle samples are too few — the analyzer
already treats `None` ambient by nulling temp_rise/theta.

**Integration:** entirely analyzer-side. `gpu_core_c`/`gpu_memjn_c`/`gpu_power_mw`
are already logged (`src/runtime/runtime_csv_rows.cpp`), ingested
(`src/analyze/analyze_csv.cpp`), and DB columns (`src/analyze/analyze_db.cpp`), so
the proxy **backfills over 100% of existing history for free** at read time. Touch
`scripts/analyze_cpu_temp_power.py` (`load_windows`, the `Window` dataclass,
`WINDOW_FIELDS`, `render_markdown`); keep `--ambient-c` as a manual
override/calibration. No schema migration, no governance gate.

**Recommend Option B (computed proxy) over a real ambient sensor**: zero hardware,
zero hot-path read, retroactive. If a real sensor is added later it feeds the
calibration offset rather than replacing the proxy.

### R2 — Per-tick workload-context tag + permanent CPU power logging (effort M)

**Why:** this is the literal "context of use." The offline analyzer bins purely by
package **watts** and treats GPU as a confound, so it cannot tell 80 W-at-idle-spike
from 80 W-during-compile from 80 W-during-AI-inference — physically different
cooling regimes get mixed. And CPU energy logging only turned on ~06-18, so the
trend that just happened is un-attributable to power. A context tag would have
explained the 06-09 -> 06-14 elevation directly (build/test churn).

**Mechanism (two coupled parts):**

1. **Permanent energy-on.** Energy logging is currently live only via the
   FEAT-0020 D-PWRLOG-1 *inversion* (the boot Safety Revert task is disabled and
   the user env is set by `scripts/Set-EnergyLoggingProfile.ps1 -Enable`) — which a
   reboot or profile reset reverts. Promote energy-on to the shipped default (env
   gate in `src/hardware/amd_reader.cpp`; retire the Safety Revert task from the
   installed set in `scripts/Build-Release.ps1`). Gate-6 already proved per-tick
   RAPL/NVML read does not move the 250 ms baseline. **Requires** amending the
   D-PWRLOG-1 decision record (governance), not new measurement.
2. **Workload tag**, computed **offline in the analyzer** (never the control path):
   ordered rules over `cpu_pkg_watts` (existing `derive_watts`), `gpu_power_mw`,
   `system_cpu_busy_pct`, `gpu_memjn_c`, optional `gpu_util_gpu_pct`, into
   `{idle, light, gaming, ai_inference, compile, cpu_heavy, unknown}` with a dwell
   gate so single-tick spikes do not create phantom windows; null inputs -> `unknown`
   (never a false bucket). Thresholds are board-specific (9950X3D + RTX5090) and
   belong in a decision doc, tuned from one labeled capture per workload.

**Integration / cost:** the tag needs `system_cpu_busy_pct` in the analyze DB,
which is deliberately excluded today (hardcoded schema; preserved patch at git tag
`archive/system-cpu-analyze-ingest`, written for schema v8). Minimal path: rebase
that patch to bump `kSchemaVersion` 12 -> 13, add `system_cpu_busy_pct` to the
`tick_samples` CREATE TABLE + a `ColumnExists`-guarded `MigrateSchema` ALTER +
`ParsedTickRow` + the ingest bind (`src/analyze/analyze_db.{h,cpp}`,
`analyze_csv.{h,cpp}`, `analyze_ingest_db.cpp`), then compute the tag in
`scripts/analyze_cpu_temp_power.py` (single source of truth) over the now-ingested
column. The runtime CSV writer already emits `system_cpu_busy_pct` — no worker
change. Old archives ingest the column as NULL -> `unknown`.

### R3 — Operator-annotation / event-marker stream (effort M)

**Why:** every attribution dead-end in this report (was it BIOS? paste? a power-plan
change? a window opened?) was invisible because the human action was never recorded.
`ambient_c` is hand-typed with no event trail.

**Mechanism:** a new CLI verb `--mark --mark-category <reseat|paste|bios|expo|
power-plan|ambient|move|note> --mark-detail "..."` (peer of `--reset-breakers`),
parsed in `src/app/app_args.cpp`, building a `RuntimeLogEvent`
`event_type="operator.marker.<category>"` and calling the existing
`AppendRuntimeEvent` (`src/runtime/runtime_event_log.cpp`) — which already appends
atomically and concurrency-safely to the live `logs/svg_mb_control_events.jsonl`.
It rides the existing event taxonomy: ingest already passes unknown keys through to
`events.extra_json`, so **no new file, no new table, no schema bump**. The analyzer
gains a `--markers` reader that tags each window with its most recent preceding
marker, so power-band tables can be sliced "before paste change" vs "after."

**Known behavior to document:** the ingest attributes events to a run by sequential
position after the last start event, so a marker dropped while the worker is stopped
gets `run_id=NULL` (still time-joinable in the analyzer). Validate category against
a fixed enum to prevent free-text sprawl.

**Precedent:** the sister GPU controller
(`D:\Development\Thermals\nvg-gpu\NVG-SmoothControl\runtime\logs\thermal_data.db`)
already implements this pattern as `load_marks` (`load_type` idle/compute,
`load_pct`, human-readable `description` such as "Cold soak", "Hard peak", "Idle
floor") plus `campaign_events`. That schema is a working template to port.

### R4 — Unified provenance view (effort M, capstone)

**Why:** decomposing a temperature today requires manually cross-referencing three
stores — temps/fans in the DB, power in late CSV rows, ambient/workload/config in
the manual ledger — and `config_sha256` is not even persisted on the `runs` row.

**Mechanism:** an analyzer-built `CREATE VIEW IF NOT EXISTS` over the existing
`(run_id, tick_count)` spine joining `tick_samples` + `tick_fan_samples` +
`tick_channel_samples` + `events`, plus two thin analyzer-owned side-tables
backfilled from data the DB currently drops: `run_provenance(config_sha256,
runtime_policy_sha256)` from the per-archive manifest, and `run_ambient(ambient_c,
workload_tag)` matched from the ledger on `(session_start, git_hash)`. Each window
then carries `{tctl/max, channel temps, fan duty/rpm, cpu_pkg_power, gpu_power,
ambient_or_proxy, workload_tag, config_sha256, operator_markers}`, so "same power
band, same ambient, same config, operator-clean" becomes a WHERE clause. No writer
migration. Degrades cleanly: pre-06-18 rows null power, pre-06-11 rows have no DB
tick, missing manifest/ledger null config/ambient.

### Suggested order

R1 first (free, retroactive, fixes the core metric). The CPU-side telemetry below
(per-CCD parse + the auto-detect fingerprint) is the priority for settings-change
detection, replacing the operator-marker idea for that purpose. R2 for the
automated "context of use" (needs the schema bump + the energy-on governance
decision). R3 markers only for un-derivable physical events. R4 last, once the
upstream signals exist to unify.

## CPU-side telemetry — what more we can read, and the auto-detect fingerprint

Feasibility was verified against the read architecture in `src/hardware/`. The
controller reads hardware read-only through PawnIO via `ioctl_read_msr` (locked to
a two-entry energy allow-list and a two-entry APERF/MPERF allow-list) and
`ioctl_read_smn` (`ReadSmnLocked`, which accepts an **arbitrary** SMN address and
is **not** allow-listed — it already serves the Tctl and per-CCD Tdie reads).

| Signal | Reachable | Effort | Hot-path cost | What it adds |
|---|---|---|---|---|
| **Per-CCD Tdie columns** (CCD1 V-cache vs CCD2 freq die) | **now** — already in `amd_sensor_summary` text for all 2.11M rows | S | none (offline parse) | the per-die picture; CCD-delta as a regime signal |
| **Effective frequency** (APERF/MPERF) | **built but off** (`CPU_CYCLES_MODE=disabled`) | M | low per-tick | MHz-per-watt, the most direct CO/PBO fingerprint |
| **Live Vcore / VSoC** (SVI telemetry) | **probe-gated** — transport exists, Zen5 SVI3 address/decode unknown | M | low per-tick | the most direct undervolt fingerprint |
| Per-core V, PPT/TDC/EDC limits | needs new module (SMU mailbox PM-table) | L | — | the PBO envelope; out of near-term scope |

**Per-CCD Tdie (do now, zero new logging).** The per-CCD values are decoded every
tick (`amd_reader.cpp` reads `ccd_base + index*4`; `ccd_base` is a static CPUID
lookup, Family 1Ah model 0x44 -> `0x00059B08`, already shipped) and serialized
into the `amd_sensor_summary` text. Parsing `CCD1 (Tdie)=` / `CCD2 (Tdie)=` into
columns is a backfillable analyzer change with no schema bump and no probe. The
per-die picture over 06-11..06-18 (this is new — `Tctl` blends/maxes the dies and
hides it):

| Day | CCD1 (V-cache) p05/p50 | CCD2 (freq) p05/p50 | (CCD2-CCD1) p50 | CCD2 hotter |
|---|---|---|---|---|
| 06-11 | 37.1 / 41.9 | 38.0 / 54.5 | +12.1 | 90% |
| 06-14 | 38.5 / 41.9 | 41.1 / 55.6 | +12.6 | 90% |
| 06-17 | 35.6 / 40.9 | 35.2 / 47.2 | +5.8 | 79% |
| 06-18 | 38.5 / 43.2 | 42.0 / 56.8 | +10.9 | 90% |

The **frequency die (CCD2) is the thermal bottleneck** (median 7-13 C hotter than
the V-cache die; `Tctl` tracks CCD2). Use the *signed* CCD2-CCD1 delta, not a fixed
ordering: CCD1 exceeds CCD2 on ~13% of ticks under cache-bound load. An asymmetric
(per-CCD) Curve Optimizer change steps this delta with no annotation.

**Effective frequency (enable the existing logger).** Setting
`CPU_CYCLES_MODE=enabled` turns on the already-built APERF/MPERF read. Caveat: it
pins **core 0 only**, so on an asymmetric dual-CCD part the ratio represents one
core, not the package, and core-0-cycles / whole-socket-Joules is a contaminated
perf-per-watt (the project already flagged this). A true package metric needs a new
all-core aggregation (sum dAPERF and dMPERF across one logical core per physical),
ideally on a dedicated off-control-thread sweep so affinity hopping never disturbs
the 250 ms loop. The existing 10.8k cycle rows predate the new BIOS/CO, so the
fingerprint cannot be validated against this change until cycles are enabled and a
post-change session is captured.

**Live Vcore (probe before promising).** The SMN transport already works on this
Zen5 part (the Zen4 CCD base carried forward and reads valid temperatures), and the
MSR allow-list does not apply to the SMN path, so a Vcore read needs **no new
module and no allow-list change** — only the correct Family-1Ah **SVI3 plane
address + decode**, which is unknown (Zen5 uses SVI3; no open driver publishes a
confirmed address, and `zenpower5` does not yet support Zen5 voltage). This is
exactly the project's probe-first case: a throwaway, default-OFF, env-gated probe
(pattern `tools/cpu_cycle_counter_probe.cpp`) sweeps `0x0005A000..0x0005A020`,
decodes candidates, and validates against HWiNFO64 with a three-part discriminator
(matches at idle and load, tracks load, and reads measurably below the pre-CO
regime at matched load). Do not assume the address or decode. Per-core voltage and
PPT/TDC/EDC limits are **not** reachable this way (they require the SMU mailbox
PM-table protocol with a version-specific struct) — a separate, larger item.

### The auto-detect CPU config fingerprint (no operator annotation)

A new analyzer module (`scripts/cpu_config_fingerprint.py`, reusing the existing
window/dwell/derive_watts machinery) detects settings changes from telemetry:

- **Config-pure dimensions** (drive segmentation; ambient/cooling-invariant):
  `idle_power_floor_w` (p05 package watts at `system_cpu_busy_pct < 5`),
  `efficiency_mhz_per_w` per load band (needs eff-freq on), `power_at_matched_busy_w`
  (watts in fixed busy bands — a CO/PBO undervolt lowers watts at the same busy%),
  `ccd_balance` (CCD2-CCD1 at matched watts), and `idle_vcore_mv` (probe-gated).
- **Cooling-output scalar, NOT a segmentation input:** `theta_c_per_w` and
  Tctl-at-matched-watt. This is the load-bearing constraint — theta is what
  analysis *measures within* a regime, so a repaste / pump / ambient shift must not
  be allowed to mint a false config regime.
- **Segmentation:** first cut on exact logged boundaries (`git_hash`,
  `config_sha256` changes — free and exact); then a numpy/stdlib median/MAD CUSUM
  step-detector on each config-pure run-level series, requiring step magnitude
  `> K*MAD` (K~4) **and** persistence over >= M subsequent runs (a BIOS change is
  set once). The 06-18 energy-on flip (`acquisition` disabled->quarantine) is
  pre-registered as a known non-config boundary so it never fires a false step.
- **Output:** auto-labeled `regime_1, regime_2, ...` (never an operator string like
  `stock-preoc`), each tagged `config-attributable` vs `cooling-change-suspected`,
  with cross-regime cooling comparisons flagged confounded. Degrades dimension-wise
  on old archives (a boundary is only detected on dimensions present on both sides).

This is the mechanism the `-25` CO change motivates: had the fingerprint been
running, the undervolt would have stepped `power_at_matched_busy_w` /
`efficiency_mhz_per_w` / `ccd_balance` and minted a new regime automatically, with
no operator action. To make it usable on the current change, enable `CPU_CYCLES_MODE`
and capture a post-change session; the per-CCD delta and (once probed) idle Vcore
sharpen it further.

## Methods and caveats

- Percentiles via `numpy` over rows fetched with `sqlite3`; pandas is not installed.
  Idle is defined two ways: busy-gated 0-10% (ledger, the trustworthy definition)
  and `cpu_tctl_c`/`gpu_core_c` p05 floors (DB, because `system_cpu_busy_pct` is not
  ingested into the analyze DB by design).
- `events.observed_temp_c` is the controller's source-mapped channel input, not raw
  `cpu_tctl_c`; channels 2/3 stayed CPU-oriented and validated against `cpu_tctl_c`
  on the overlap (corr 0.90, +1.3 C bias). The source mapping changed on 06-09
  (radiator GPU-assist deployment), so cross-06-09 comparison uses the CPU-oriented
  channels only.
- n=8 days in the DB window is small; the ambient ruling rests on R^2=0.000 plus
  anti-correlated rank ordering plus the stable GPU sensor offset, not a single
  statistic.
- 06-19/06-20 are outside the DB (span ends 06-18 18:38); those points rely on the
  ledger and were not re-verified at tick resolution.
- The GPU idle floor is an internal proxy for intake air, valid only because its
  sensor offset proved constant; there is no room thermometer in the data.
