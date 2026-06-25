# FEAT-0006 §12 loop-timing gate — all-core off-thread sweeper M-evidence (2026-06-25)

**Verdict: PASS (provisional tolerance).** Enabling the all-core off-thread cycle
sweeper (`SVG_MB_CONTROL_CPU_CYCLES_MODE=enabled`) does **not** move the shipped
250 ms control-loop profile. The discriminator `loop_work_duration_ms` p99-of-bulk
is **lower** in the sweeper-ON candidate than in either of two sweeper-OFF
baselines, in every CPU phase, and the low-noise idle-phase median confirms the
sweeper adds no measurable per-tick cost (< 0.1 ms, > 2000× below the 250 ms
budget). This closes the §12 measurement-gate obligation that gates promoting any
enabled-live all-core evidence out of `quarantine`.

This is the FEAT-0020 gate-6 analog for the new sweeper thread. Per FEAT-0006 §12
the verdict is **provisional**: the tolerance is calibrated from a single
off-vs-off pair (n=2 baselines), not a validated threshold.

## What was measured

The all-core roll-up runs a dedicated **off-thread** sweeper (its own thread +
PawnIO handle, affinity restored via RAII) so its per-core APERF/MPERF reads never
execute on the 250 ms control thread (FEAT-0006 §14, PRs #25/#26). The gate asks
one question before that path's enabled-live evidence may leave `quarantine`: does
the sweeper's PawnIO activity perturb the control loop's own per-tick work?

`SVG_MB_CONTROL_CPU_CYCLES_MODE=enabled` turns on **both** the per-core read and
the all-core sweeper (no sweeper-only flag), so the gate bounds their combined
per-tick cost vs cycles-OFF — conservative; the sweeper is the new element §12
targets.

## Captures

Three attended captures via `scripts/Capture-EnergySession.ps1` on the live box,
operator present, energy `enabled` throughout (the live D-PWRLOG-1 steady state),
28-thread CPU synthetic load (`cpu-synth-load.exe`), profile idle 300 s → load
720 s → cooldown 300 s. The worker restart at each capture's enable step rotates
the live CSV, so each `session.csv` is its own clean ~22.2-min window (verified
below).

| Run | Dir | Cycles (sweeper) | rows | window (snapshot_time) | span |
|---|---|---|---|---|---|
| A | `sweeper-off-A` | OFF (baseline) | 5176 | 04:34:41 → 04:56:54 | 22.2 min |
| B | `sweeper-off-B` | OFF (baseline, calibration) | 5072 | 05:00:33 → 05:22:46 | 22.2 min |
| C | `sweeper-on` | **ON (candidate)** | 5268 | 05:32:27 → 05:54:40 | 22.2 min |

Dirs under `release\runtime\experiments\energy-quarantine\`.

- **Build provenance:** live binary SHA `65972F12…`, `builtUtc 2026-06-23T06:24:56Z`,
  `workingTreeDirty: false`, `testsPassed: true` — after the all-core-sweeper merge
  (PRs #25/#26), so no new build was required. The all-core columns are present in
  the live CSV header; the sweeper ran (confirmed below).
- **Environment regime:** GPU in low-power desktop state throughout —
  `gpu_power_mw` p50 ≈ 96 / 97 / 100 W (A/B/C), max ≈ 124 / 117 / 127 W, **all rows
  < 150 W** so every tick lands in the gate's single "idle" GPU bucket. The GPU
  power is consistent across runs (no divergent confound), so the GPU bucketing is
  effectively inert here and the comparison is like-for-like. This evidence speaks
  to the **GPU-idle / low-power regime**; the GPU-busy regime was not exercised (no
  in-repo GPU load tool; operator-supplied). Per the §12 docstring the perturbation
  mechanism is *PawnIO driver contention*, present regardless of GPU state, so
  GPU-idle is a valid and conservative regime for this gate.

## Sweeper-ran pre-gate (a PASS on a candidate that never ran the sweeper is meaningless)

In `sweeper-on/session.csv`: `cpu_cycles_acquisition = quarantine` on 5268/5268
rows; `cpu_cycles_allcore_cores > 0` on **5084/5084** cycle rows, every one = 32
cores, with real, varying `cpu_aperf_delta_allcore` / `cpu_mperf_delta_allcore` /
`cpu_cycles_window_ms_allcore` (1058 distinct APERF-delta values over 1076 distinct
off-thread sample IDs; APERF/MPERF ratio 1.15–1.29). Both OFF baselines:
`cpu_cycles_acquisition = disabled`, zero all-core rows. The sweeper genuinely ran
in C and was genuinely off in A/B.

## Calibration — off-vs-off variance (the real tolerance)

`score_loop_timing_gate.py --baseline off-A --candidate off-B` (idle GPU bucket /
"all" rollup):

| metric | off-A | off-B | off-vs-off drift |
|---|---|---|---|
| p99_bulk `loop_work_duration_ms` | 86.28 ms | 81.56 ms | **4.72 ms / 5.5%** (B below A) |
| spikes > 100 ms | 149 | 121 | context |
| overruns > 250 ms | 37 | 30 | context |
| max | 1963 ms | 3578 ms | context (single environmental stalls) |

The whole-window p99-bulk has ~4.7 ms run-to-run noise, dominated by the 12-min
load phase. (This is far above the script-header's ~0.03 ms 2026-06-21 idle-bucket
reference because those calibration captures had little load-phase starvation;
these full 28-thread runs do.) The default tolerance (`rel_tol=0.5`) is far too
loose — it would allow a +43 ms move — so the gate was also scored with a
calibrated `rel_tol=0.12, abs_tol_ms=10` (≈ 2× observed drift).

## §12 gate result — OFF vs ON

| comparison | tolerance | base p99_bulk | cand p99_bulk | overruns b→c | verdict |
|---|---|---|---|---|---|
| off-A vs ON | default (0.5 / 2) | 86.28 | **72.15** | 37 → 8 | PASS, 0 moved |
| off-A vs ON | calibrated (0.12 / 10) | 86.28 | **72.15** | 37 → 8 | PASS, 0 moved |
| off-B vs ON | calibrated (0.12 / 10) | 81.56 | **72.15** | 30 → 8 | PASS, 0 moved |

The sweeper-ON candidate's p99-bulk (72.15 ms) is **below both** OFF baselines,
with fewer overruns (8 vs 37/30) and lower max (1203 vs 1963/3578). PASS regardless
of tolerance; 0 buckets moved.

## CPU-phase split — the sensitive test

The whole-window gate is dominated by load-phase CPU-starvation noise and cannot,
by itself, distinguish "sweeper adds nothing" from "C's window was quieter." The
phase split (using the manifest phase timestamps) isolates the low-noise idle
phase, where the control thread is not starved and a fixed PawnIO cost would show
clearly. `loop_work_duration_ms` per phase:

| phase | off-A p99_bulk | off-B p99_bulk | **ON p99_bulk** | off-A p50 | off-B p50 | **ON p50** |
|---|---|---|---|---|---|---|
| idle (sensitive) | 55.8 | 62.9 | **58.3** (in-band) | 2.94 | 2.85 | **2.779** (lowest) |
| load_steady | 95.1 | 84.6 | **85.8** (in-band) | 8.38 | 3.95 | **5.94** (in-band) |
| cooldown | 56.0 | 61.6 | **56.5** (in-band) | 3.10 | 3.00 | **3.09** |

In **every** phase the sweeper-ON candidate lands inside the off-vs-off variance
band — no phase where ON exceeds both OFF runs. The strongest statement is at the
**median** (low variance: off-vs-off idle p50 floor = 0.091 ms): ON's idle p50 is
**2.779 ms, the lowest of the three** and 0.069 ms below the lower OFF baseline. Any
per-tick median cost from the off-thread sweeper is below the ~0.1 ms off-vs-off
noise floor — i.e. no measurable cost.

**Why the idle-phase null also bounds the load phase.** The off-thread sweeper
reads all 32 logical processors every ~1 s identically whether the CPU is idle or
loaded — its per-tick PawnIO work is phase-independent. There is no mechanism by
which a sweeper-induced contention cost on the control thread's own read would
appear *only* under CPU load. So the idle-phase null (ON flat against a 0.091 ms
off-vs-off floor) bounds the load-phase effect too, even though the load phase —
where the gate's whole-window noise (~4 ms p50) is least able to resolve a small
effect directly — is where contention would be highest. This closes the one fair
sensitivity objection (the load phase being the least-sensitive window).

## Criterion-4 (cycle effective-frequency validity) — MANUAL, plausible

`score_energy_session.py --manifest sweeper-on --p0-mhz 4300`: **5 PASS / 0 FAIL /
1 MANUAL** (criterion 4, as designed without an Option-B locked clock).

- Effective frequency (all-core source): dAPERF/dMPERF idle 1.242 / load 1.228 →
  **derived idle 5339 MHz, load 5278 MHz @ P0 4300**. Plausible for a 9950X3D under
  28-thread PBO load (between base 4.3 GHz and the ~5.7 GHz single-core ceiling;
  idle > load is correct AMD clock-while-running semantics). Confirmed three
  convergent ways in verification: ratio-of-sums 5279, direct ΣAPERF/(32·t) 5222,
  core-0 ratio 5266 MHz — the direct check rules out a fake/core-0-replicated
  all-core read.
- Energy criteria sane: counter continuity (0 negative deltas, 1.82 wraps over
  119 476 J), plausible range (idle 67.5 W / load 165.7 W), **SMU cross-check
  +0.5%** (RAPL 165.9 W vs HWiNFO SMU 165.0 W, 412 samples), no false zeros across
  5268 quarantine rows. Full energy criteria are in the auto-scored session-4 note
  [`docs/cpu-energy-quarantine-exit-evidence-2026-06-25-s4.md`](cpu-energy-quarantine-exit-evidence-2026-06-25-s4.md)
  — the first quarantine-exit session captured with the all-core sweeper enabled.

Criterion-4 stays **MANUAL** (the standing status): a derived effective MHz without
an external locked-clock reference is plausible, not formally validated. Promoting
it needs an Option-B locked-clock capture, out of scope here.

## Adversarial verification

A 6-skeptic workflow independently re-derived every number from the raw CSVs (not
trusting the orchestrator's figures), each attacking the PASS along a distinct
failure mode. **All six returned `pass_holds`** (5 severity `none`, 1 `low`):

| angle | verdict | independent finding |
|---|---|---|
| window integrity | pass_holds | windows temporally disjoint (gaps 219/581/2133 s), 0 cross-window rows, 0 shared timestamps — not a near-identical-files artifact |
| sweeper-actually-ran | pass_holds | ON 5084 rows × 32 cores, real varying APERF/MPERF; OFF disabled, 0 rows |
| threshold masking | pass_holds | ON ≤ both baselines in every band (50–100/90–100/95–100/>100/>250 ms), no pile-up at the 100 ms wall |
| sensitivity floor | pass_holds (low) | gate is structurally coarse (~10 ms p99_bulk MDE, never inspects p50); the phase-split p50 is the sensitive analysis and shows no movement |
| direction fluke | pass_holds | median corroborates: ON idle p50 2.779 < both OFF; not a spike-count fluke |
| crit-4 / energy validity | pass_holds | effective freq confirmed 3 ways; direct-APERF rules out a fake read; SMU +0.5% vs 412 HWiNFO samples |

## Honest limitations

- **Provisional tolerance / n=2 baselines.** The off-vs-off variance is one spread
  estimate, not a distribution. The idle-phase p50 floor (0.091 ms) is tight and ON
  is below it, but a larger baseline set would harden the threshold.
- **Gate tooling is coarse (LOW finding).** The shipped `score_loop_timing_gate.py`
  uses p99-of-bulk only (~10 ms calibrated MDE) and never inspects p50/mean; under
  GPU-idle its GPU bucketing collapses to one bucket and adds no discrimination. The
  **wall-clock CPU-phase split + median**, performed here outside the gate, is what
  supplies the sensitivity. Folding a phase-split / p50 check into the gate tooling
  is a worthwhile non-blocking follow-up.
- **GPU-idle regime only.** The GPU-busy buckets (mid/load) were never populated; a
  GPU-busy capture would extend coverage. The §12 mechanism (PawnIO contention) is
  GPU-independent, so this does not weaken the GPU-idle conclusion.

## Marker decision (governance) — RECORDED 2026-06-25

The §12 sweeper gate — the stated precondition for any enabled-live all-core
evidence to leave `quarantine` — is **PASS**. **Maintainer decision recorded
2026-06-25: the cycle/all-core acquisition path (`cpu_cycles_acquisition`) is
promoted `quarantine → validated`.** This is a recorded evaluation outcome, not a
code/CSV change — the logged marker stays `quarantine` in data, the worker never
auto-sets `validated`, and the analyzer does not branch on it. The decision and its
mapping to the §Evaluation criteria are in
[`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`](cpu-work-energy-acquisition-decision-2026-06-07.md)
(§Quarantine-exit decision — cycle/all-core path). Decision-doc §4 (effective-freq
validity) is met by plausibility + affinity stability; the scorer's MANUAL label is
its stricter *optional* Option-B locked-clock cross-check, a future strengthening,
not a blocker. The package-energy marker is a separate maintainer decision (its
evidence is complete; not promoted here).

## Steady state after the captures

Each capture's `finally` block reverts to the pre-run env and restarts the worker;
the "marker did not return to 'disabled'" warning is expected/benign (energy is
intentionally left live-`enabled`). Confirmed after run C: `health_state = healthy`,
energy `enabled` (marker `quarantine`), cycles `disabled` (marker `disabled`) — the
D-PWRLOG-1 steady state — 0 degraded channels, no capture processes lingering.

## Reproduce

```
# from repo root, captures via (elevated):
.\scripts\Capture-EnergySession.ps1 -EnergyOnly -LoadThreads 28 -SynthLoadExe release\runtime\experiments\energy-quarantine\cpu-synth-load.exe -OutDir <dir>\sweeper-off-A -SessionLabel sweeper-off-A
.\scripts\Capture-EnergySession.ps1 -EnergyOnly -LoadThreads 28 ... -OutDir <dir>\sweeper-off-B
.\scripts\Capture-EnergySession.ps1            -LoadThreads 28 ... -OutDir <dir>\sweeper-on
# score:
python scripts\score_loop_timing_gate.py --baseline <off-A>\session.csv --candidate <off-B>\session.csv          # calibrate
python scripts\score_loop_timing_gate.py --baseline <off-A>\session.csv --candidate <sweeper-on>\session.csv --rel-tol 0.12 --abs-tol-ms 10
python scripts\score_energy_session.py --manifest <sweeper-on>\manifest.json --session-num 4 --p0-mhz 4300        # criterion-4
```
