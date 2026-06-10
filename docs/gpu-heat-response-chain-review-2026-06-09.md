# GPU-Heat Fan-Response — Chain Review and Simulation (2026-06-09)

Status: **review / analysis** (not a contract). Reviews how every controlled
fan responds to GPU heat, simulates the response, and tests the shipped docs
against the shipped numbers rather than assuming the docs are correct or that
the deployed behavior is the intended one.

- Config under review: `config/control.release.json` (schema_version 4).
- Source of truth for the chain: `src/control/*.cpp` (cited inline), cross-read
  against `docs/CONTROL_PIPELINE_MATH.md`.
- Simulation tool (throwaway, not committed by default):
  `tools/gpu_response_sim.py` + `tools/gpu_response_extra.py`. Faithful
  tick-by-tick Python port of the shipped pipeline that loads the shipped
  config directly (no hand-transcribed curves).
- Verification: the port's boost integrator reproduces the C++ tested golden
  (`tests/cpp/boost_stage_tests.cpp`) to 0.0; five sampled setpoints were
  re-derived by hand and by two independent agents and match within 0.3
  duty-points.

---

## 0. Scope and assumptions (read first — the results are bounded by these)

1. **"All fans" = the six controlled channels 0–5.** Channel 6 (CHA6,
   AIO/pump) `is` excluded from control and from case-pressure math
   (`snd-desk.cooling.policy.json` `excluded_from_case_pressure`).
2. **GPU "temperature" `is` the control envelope**
   `max(core_c, memjn_c, hotspot_c if > 0)`
   (`channel_evaluator.cpp:402` `GpuControlEnvelopeC`). The sim sweeps this
   single scalar. Live `core`, `memjn`, and `hotspot` can diverge by 10–20 °C;
   this review does **not** characterize which sensor dominates the envelope at
   runtime. The controller's GPU input in practice is usually `memjn`
   (memory-junction), the slow envelope.
3. **CPU `is` held at 40 °C** to isolate the GPU response. This reading `is`
   only valid while CPU < `source_aware_cpu_hot_guard_c` = **75 °C**. At/above
   75 °C the selection changes (§7).
4. **Output `is` duty %, not RPM.** The policy's `flow_index` is
   `rpm/1000 · (diameter/120)²` — RPM-based and diameter-weighted (200 mm
   intakes vs 140 mm exhausts). **Duty ordering is not flow ordering**, so this
   review does not, and cannot, validate the "positive case pressure" claim
   from duty alone (§6, caveat C1).
5. Steady-duty tables are reported at a **10-minute hold** (realistic dwell)
   and a **60-minute hold** (near-asymptotic). They differ in the 64–68 °C
   band because the boosts ramp slowly there (§5.3) — the steady table is
   measurement-window-dependent in that band.

---

## 1. The chain: how GPU heat reaches each fan

Per control tick (250 ms), for a channel with
`temp_blend = max_cpu_gpu_source_aware` (all six channels in the shipped
config), with CPU cool and GPU available:

```
 GPU core/memjn/hotspot
        │  GpuControlEnvelopeC = max(core, memjn, hotspot>0)      [channel_evaluator.cpp:402]
        ▼
 T_gpu (envelope)
        │  SelectPrimaryCurveInput: CPU<75 guard & GPU avail  ->  primary = T_gpu  (source="gpu")
        │                                                          [channel_evaluator.cpp:219-234]
        ▼
 raw = LookupCurve(channel.curve, T_gpu, min_duty, smootherstep)   [control_policy.cpp:68]
        │  floored to min_duty, clipped to 100
        ▼
 raw = max(raw, LookupCurve(cpu_override_curve, T_cpu, ...))        [channel_evaluator.cpp:287]
        │  CPU=40 -> override is at/near floor -> primary (GPU) wins; observed_temp = T_gpu
        ▼
 smoothed = EMA(raw)  (rise/fall alpha + decay latch)               [channel_evaluator.cpp:72]
        ▼
 + thermal_pressure(observed=T_gpu)   start 84-86 °C                ┐  boost_stage.cpp:51
 + midband_pressure(observed=T_gpu)   start 64-66 °C                │  integrators, additive
 + gpu_airflow(T_gpu envelope)        start 62-64 °C                │  (each rises to its
 + cpu_low_soak(T_cpu)  ≈ 0 here      start 72 °C (ch1/4/5 only)    ┘   max while T≥start)
 + low_band  ≈ 0 here  (vetoed while any boost>0.05; capped at 1.0%) [low_band_integrator.cpp]
        ▼
 desired = clip(smoothed + Σboosts, 0, 100)                         [channel_evaluator.cpp:353]
        ▼
 setpoint = rate_limit(desired)  (rise/fall %/min, max step)        [channel_evaluator.cpp:37]
        ▼
 gates: deadband (0.25%), cooldown (250 ms), policy, breaker  ->  fan write
```

**Three independent observations about this chain, none of which the prose
docs make explicit:**

- **The per-channel `curve` is the GPU response curve.** Because the
  source-aware selection feeds `T_gpu` into `LookupCurve(channel.curve, …)`
  when CPU is cool, the array named `curve` (not `cpu_override_curve`) is what
  shapes the graded GPU response. The `cpu_override_curve` is the CPU response.
- **`midband_pressure` is a second GPU term**, despite its non-GPU name. Its
  input is `observed_temp`, which `is` `T_gpu` in this regime
  (`boost_stage.h:64`, `ObservedTemp`). It contributes up to 6–10 % and starts
  at 64–66 °C — it dominates the boost stack over `gpu_airflow` (max 4–8 %).
- **`thermal_pressure` also keys off `T_gpu`** here (input `observed_temp`),
  so at extreme GPU (≥84–86 °C) it stacks too — even though it was designed as
  CPU-spike headroom (see Finding D1).

### 1.1 GPU-driven terms by channel (from `control.release.json`)

| Ch | role | gpu_airflow (start / max%) | midband (start / max%) | thermal (start / max%) |
|----|------|---|---|---|
| 0 | rear_exhaust | 64 / 4 | 64 / 8 | 86 / 6 |
| 1 | radiator_exhaust_a | 64 / 5 | 64 / 10 | 85.5 / 20 |
| 2 | front_intake_200 (L) | 62 / 8 | 66 / 6 | 86 / 4 |
| 3 | front_intake_200 (R) | 62 / 8 | 66 / 6 | 86 / 4 |
| 4 | front_radiator_intake | 64 / 5 | 64 / 10 | 84 / 14 |
| 5 | radiator_exhaust_b | 64 / 5 | 64 / 10 | 85.5 / 20 |

`gpu_airflow` uses the GPU envelope directly; `midband`/`thermal` use
`observed_temp` (= GPU here). `cpu_low_soak` (ch1/4/5, start 72 °C, max 0.3 %)
is CPU-only and ≈ 0 in this scenario. **Onset order: gpu_airflow (62–64) <
midband (64–66) ≪ thermal (84–86).**

---

## 2. Simulated steady duty vs GPU envelope (CPU 40 °C)

10-minute hold (realistic dwell). Intakes left, exhausts right.

| GPU °C | ch2 int | ch3 int | ch4 radint | ch0 rear | ch1 radexh | ch5 radexh |
|---:|---:|---:|---:|---:|---:|---:|
| 40 | 42.8 | 38.8 | 24.6 | 15.5 | 22.0 | 20.0 |
| 50 | 46.0 | 42.0 | 27.0 | 15.5 | 22.0 | 20.0 |
| 55 | 48.8 | 44.8 | 28.4 | 15.5 | 23.0 | 20.5 |
| 60 | 53.7 | 49.7 | 30.9 | 15.5 | 24.0 | 21.0 |
| 65 | 63.6 | 59.6 | 33.3 | 16.8 | 28.1 | 25.1 |
| 70 | 77.4 | 73.4 | 52.6 | 33.3 | 45.2 | 42.2 |
| 75 | 80.1 | 76.1 | 54.3 | 36.0 | 48.5 | 45.0 |
| 80 | 93.8 | 89.8 | 64.4 | 45.3 | 53.0 | 49.0 |
| 85 | 98.1 | 94.1 | 72.5 | 48.6 | 60.7 | 57.7 |
| 90 | 100  | 100  | 95.0 | 67.1 | 87.0 | 84.0 |
| 95 | 100  | 100  | 100  | 74.0 | 94.0 | 91.0 |

Reading: the **200 mm intakes lead** at all temperatures and saturate by
~85 °C; the **front radiator intake (ch4)** and the **three exhausts** stay
much lower until ~66 °C, then climb. Contribution split at GPU 80 °C
(10-min): ch5 = 34 (curve) + 10 (midband) + 5 (gpu_airflow) = 49; ch1 =
38 + 10 + 5 = 53; ch0 = 33.3 + 8 + 4 = 45.3; ch2 = 84.1 + 6 + 8 = 98.1.

---

## 3. Onset grid — where each fan actually starts (CPU 40 °C)

Duty at 10-min hold, `[60-min]` in brackets where it differs.

| GPU °C | ch2 | ch3 | ch4 | ch0 | ch1 | ch5 |
|---:|---|---|---|---|---|---|
| 60 | 53.7 | 49.7 | 30.9 | 15.5 | 24.0 | 21.0 |
| 62 | 54.0 | 50.0 | 31.0 | 15.5 | 24.6 | 21.6 |
| 64 | 61.5 [62.6] | 57.5 [58.6] | 31.4 | 15.5 | 26.4 | 23.4 |
| 66 | 65.2 | 61.2 | 39.5 [45.9] | 21.5 [27.9] | 33.3 [39.7] | 30.3 [36.7] |
| 68 | 70.6 [74.8] | 66.6 [70.8] | 49.5 [50.8] | 30.8 | 41.5 [42.8] | 38.5 [39.8] |
| 70 | 77.4 | 73.4 | 52.6 | 33.3 | 45.2 | 42.2 |

**The exhausts (ch0/ch1/ch5) and the radiator intake (ch4) are effectively at
their floor until ~64–66 °C**, then ramp steeply through 66–70 °C. The 200 mm
intakes start climbing earlier (their `curve` rises from ~50 °C) and jump again
at 64 °C when their boosts engage.

---

## 4. Transient (step) response: GPU 35 → 80 °C, CPU 40 °C

| channel | t=35 °C | 30 s | 60 s | 120 s | →20 min | reach within 1 pt |
|---|---:|---:|---:|---:|---:|---:|
| ch2 intake | 42.8 | 65.7 | 88.2 | 93.8 | 93.8 | 66 s |
| ch3 intake | 38.8 | 61.7 | 84.2 | 89.8 | 89.8 | 66 s |
| ch4 rad-int | 24.6 | 39.7 | 54.7 | 63.9 | 64.4 | 100 s |
| ch0 rear | 15.5 | 34.3 | 44.3 | 45.3 | 45.3 | 60 s |
| ch1 rad-exh | 22.0 | 40.7 | 52.1 | 53.0 | 53.0 | 58 s |
| ch5 rad-exh | 20.0 | 38.7 | 48.2 | 49.0 | 49.0 | 55 s |

The combined EMA smoothing + per-minute rate limit gives a **~1-minute rise to
target** on a GPU thermal step. Transient GPU spikes shorter than ~30 s are
heavily damped (good for noise; means brief spikes barely move the fans).

---

## 5. Findings — testing the docs and the design

Severity and kind are the reviewer's; recommendations are advisory.

### Docs vs numbers

**F-DOC1 [high] — Exhaust GPU onset is ~64 °C, documented as "~60 °C".**
`snd-desk.cooling.policy.json` (`radiator_exhaust_pair.gpu_airflow_assist`,
ch1/ch5 `gpu_airflow_note`) and `COOLING_STRATEGY.md` say the exhausts ramp
"gentle from ~60 C." In the shipped config both GPU-driven boosts start at
**64 °C** (`gpu_airflow_start_c` = `midband_pressure_start_c` = 64 on ch1/ch5),
and the `curve` at 60 °C gives only 21–24 % (≈ floor). At 60 °C there is no
meaningful exhaust response. *Recommendation: change the prose to "~64 °C", or
— if 60 °C was the intent — lower the two `*_start_c` to 60 in config. This is a
doc-vs-config conflict; the maintainer owns which side is wrong.*

**F-DOC2 [medium] — "stronger toward ~80 °C" overstates the 80 °C point.**
At GPU 80 °C the exhausts are ~49–53 % (about half their ceiling). "Strong"
(84–87 %) is not reached until ~90 °C. *Recommendation: restate as a
progression — negligible <64, steep 66–70, moderate 70–84, strong 84–90+ — or
say "toward ~90 °C".*

**F-DOC3 [medium] — The exhaust GPU response is composite, not just
"gpu_airflow".** The docs name a "GPU-airflow assist curve" (max 5 %), but the
larger GPU term on ch1/ch5 is `midband_pressure` (max 10 %), plus the `curve`
itself. *Recommendation: document the exhaust GPU response as curve +
midband(10 %) + gpu_airflow(5 %).*

### Is this wanted (design questions for the maintainer)

**F-DES1 [high] — `thermal_pressure` now fires on GPU heat for the CPU-radiator
exhausts.** `thermal_pressure` was designed as CPU-spike headroom (radiator-
coupled, transient). Because ch1/ch5 are now source-aware with `observed_temp`
= GPU below the 75 °C guard, `thermal_pressure` ignites on **GPU** alone at
≥85.5 °C while the CPU and coolant are cool. At GPU 90 °C it contributes its
full 20 points (ch1 → 87 %, ch5 → 84 %). The decision doc says overlays are
"preserved when CPU is the driving signal" but does not note they now also fire
on GPU. *Recommendation: decide deliberately — (A) leave thermal/midband CPU-
only on these channels, (B) cap them lower on the GPU path, or (C) accept and
rename as case-pressure headroom and document it.*

> **Resolved 2026-06-10 (maintainer): option (C)** — accepted as case-pressure
> headroom, documented in `docs/COOLING_STRATEGY.md` (Radiator Exhaust Pair).
> Rationale: the ch0-vs-ch1/ch5 inversion that motivated capping was fixed by
> the 2026-06-09 retune (ch0 reaches parity under load); a GPU memory-junction
> envelope `>= 85.5 C` is uncommon on this build; no case-air sensor is logged,
> so the empirical "validate against case-air-temperature" leg (F-DES3, C1) is
> closed and the call is on merits; and `thermal_pressure` is shared with the
> CPU path, so capping it on the GPU path risks the CPU response the retune
> deliberately preserved. `config/control.release.json` is unchanged.

**F-DES2 [medium, open question] — ch4 (front radiator intake) runs ~95 % on
GPU-only heat — is that intended?** At GPU 90 °C / CPU 40 °C, ch4 reaches 95 %
while the CPU radiator it feeds is cool. Two readings exist and this review
cannot pick between them from code alone: (a) as a front intake, ch4 still
delivers fresh case airflow over the GPU and raises positive pressure, which
the strategy wants at high GPU load — so the ramp may be fine; (b) if ch4
shares a push/pull core with the ch1/ch5 exhausts, ramping all three on GPU
adds to both sides and does little net. (ch4 was already source-aware before
the 2026-06-09 change; only ch1/ch5 changed then.) *Recommendation: confirm the
radiator push/pull topology and whether ch4's GPU ramp is wanted; do not assume
either answer. The repo does not record the physical fan↔core wiring.*

**F-DES3 [medium] — Sustained GPU-only exhaust duty (84–87 % at GPU 90) is high
and audible.** `thermal_pressure` was tuned for seconds-long CPU transients;
here it saturates at 20 % and holds for minutes. *Recommendation: measure
whether GPU-driven exhausts actually lower intake-air temperature vs CPU-only;
if the gain is small, revert ch1/ch5 thermal_pressure to CPU input, or cap it.*

### Caveats that must accompany any use of these numbers

**C1 [high] — Duty is not flow.** The "positive case pressure / intake > exhaust
until GPU 68 °C" claim cannot be validated from this sim. Converting duty → RPM →
`flow_index` (diameter-weighted) is required and is not modeled. The idle intake-
bias is validated separately at *reference RPM* in
`tests/test_machine_cooling_policy.py`, not here.

**C2 [high] — Single-scalar GPU.** Sweeping one envelope value hides core/memjn/
hotspot divergence (10–20 °C live). The 68 °C pressure-relax gate may trigger on
a different sensor at runtime than the swept value implies. This is a coherent-
point scenario, not a sensitivity analysis.

**C3 [medium] — Dwell-dependence in the 64–68 °C band.** Just above a boost's
`start_c` the smootherstep ramp rate is ~0 (≈0.009 at 65 °C vs ~0.5 at 70 °C),
so the boost takes ~48 min to saturate at 65 °C but ~36 s at 70 °C. The **same
GPU temperature yields different steady duty depending on how long it has been
held** (e.g. GPU 66 °C: ch5 = 30.3 % at 10 min, 36.7 % at 60 min). The creep is
upward only (thermally conservative) and is not noted in the main control docs.

### Lower-severity / informational

- **F-GUARD [info] — 75 °C guard is a clean, intentional switch.** Below 75 °C
  CPU, primary = GPU (`source="gpu"`); at/above, selection becomes
  `max(CPU,GPU)` (`source="cpu_guard"`) and `cpu_override_curve(CPU)` lifts duty.
  Guard scan at GPU 72 °C: CPU 74 → ch2 = 78.0 (gpu); CPU 76 → 82.2 (cpu_guard);
  CPU 80 → 93.8. Rate-limited, no instantaneous jump on hardware.
- **F-ANOM1 [low] — ch1/ch5 `cpu_override` inversion at 88 °C** (ch5 = 61 vs
  ch1 = 60). The "not mirrored" stagger contract applies only to the GPU
  `curve`; the CPU-override curves are near-identical by design and run in
  unison under CPU emergency. Minor hand-tuning artifact, emergency band.
- **F-OK [info] — Config structurally sound.** All primary and override curves
  are monotonic; every boost band has `start_c < full_c`. The `thermal_pressure`
  max asymmetry (ch1/ch5 = 20 % vs intakes 4–6 %) is intentional (radiator
  spike headroom).

---

## 6. Verification status

What is numerically pinned, and what is not:

- **Boost integrator (pinned):** the Python port reproduces the C++ tested
  golden (`boost_stage_tests.cpp` legacy reference) to max abs diff 0.0.
- **Curve + steady-state compose (pinned):** five setpoints re-derived by hand
  and by two independent agents from `control.release.json` +
  `CONTROL_PIPELINE_MATH.md` match the sim within 0.3 pt (e.g. GPU 80 →
  ch5 = 49.0, ch1 = 53.0, ch0 = 45.3; GPU 75 → ch4 = 54.25). These checks
  converge regardless of the smoothing path, so they do **not** exercise it.
- **Transient path (faithful-by-inspection, NOT independently pinned):** §4
  (step shape, ~1-min rise) and C3 (dwell-dependence) come from the EMA +
  rate-limiter + deadband/cooldown ports of `ApplyDemandSmoothing`,
  `RateLimitSetpoint`, and the write gates. These were ported line-for-line but
  not pinned to a tested golden. As a sanity bound, ch2's EMA alpha 0.018 alone
  predicts ~54 s to close to 1/51 of the step gap (≈ 216 ticks); the sim's
  ~66 s (EMA plus the looser rate limiter) is consistent. Treat the transient
  numbers as model predictions whose generator is unverified end-to-end.
- **Not measured:** no hardware/CSV trace of a GPU-only load at this config was
  available, so §2–§4 are predictions, not observations. A live GPU-load
  capture (logging core, memjn, hotspot separately) `should` confirm §2–§4 and
  resolve C1/C2 — and would be the place to numerically pin the transient path.

## 7. Open questions for the maintainer

1. Is "~60 °C" the intended exhaust onset (fix config) or a stale doc (fix prose)? (F-DOC1)
2. Should `thermal_pressure`/`midband_pressure` fire on GPU heat at all for the
   CPU-radiator exhausts ch1/ch5? (F-DES1) — **Resolved 2026-06-10: (C) accept +
   document** (see F-DES1 above; `docs/COOLING_STRATEGY.md`).
3. Is GPU-driving the radiator **intake** ch4 coherent with the back-pressure-
   relief rationale? (F-DES2)
4. Is 84–87 % sustained exhaust duty on GPU-only load acceptable for noise/power,
   or should the GPU path be capped? (F-DES3)

---

## 8. Reconciliation + no-bias merits review (2026-06-09, follow-up)

### 8.1 Step 1 — F-DOC1/2/3 reconciled (docs only)

`COOLING_STRATEGY.md` and `snd-desk.cooling.policy.json` were corrected to
**describe the shipped behavior**: GPU-driven boosts start at **64 °C**
(not "~60 °C"); the per-channel `curve` adds only ~1–2 pts over floor below
64 °C; the ramp is steepest through **66–70 °C**; the response is **~half
strength near 80 °C and strong toward ~90 °C** (not "stronger toward ~80 °C").
The exhaust GPU response is named as composite (`curve` + `midband_pressure`
max 10 % + `gpu_airflow` max 5 %). **The live `control.release.json` was not
changed** — lowering `*_start_c` to 60 would be a deployed-cooling change and
is governance-gated; whether the onset *should* move is judged below on merits,
not by deferring to the old prose.

### 8.2 Step 2 — reviewing the behavior with no bias from the docs

Reasoning from the actual goal (case fans assist the GPU's own fans by feeding
it cooler intake air and exhausting GPU-heated case air) and the sim, not from
any doc claim:

- **Direction is coherent.** Under GPU-only load the GPU dumps heat into the
  case, so ramping front intakes (ch2/ch3, cool air toward the GPU) and exhausts
  (ch0/ch1/ch5, removing GPU-heated air) on GPU heat is the right sign for every
  controlled lane. **F-DES2 is downgraded:** ch4 as a front-radiator *intake*
  still delivers cool case airflow even with the radiator cool, so its GPU ramp
  is defensible, not clearly wasteful. (The push/pull-topology question remains
  open but is lower priority.)

- **Onset 64 °C is fine; 60-vs-64 is immaterial.** The controller's GPU input is
  `memjn` (memory junction), which runs hot/lags; a 64 °C memjn onset begins case
  assist at light–moderate GPU load, which matches intent. The 4 °C the old doc
  was off by is within tuning noise and is **not** a reason to change the config.

- **Top merits concern (sharpened F-DES1): the exhaust allocation is inverted
  for GPU heat.** At memjn 90 °C the sim gives the *restrictive* radiator
  exhausts ch1 = 87 % / ch5 = 84 % but the *unobstructed* rear exhaust ch0 only
  67 %. The free-flowing lane that most effectively evacuates GPU-heated case air
  is driven the *least*, because ch1/ch5 carry `thermal_pressure` max **20 %**
  (CPU-spike headroom, now firing on GPU envelope ≥85.5 °C) while ch0 carries
  only **6 %**. memjn ≥ 85.5 °C is common on GDDR6X under real gaming load, so
  this is not a corner case. Paying 20 sustained points of noise to push a *cool,
  restrictive* radiator harder than the open rear vent is questionable for GPU
  case-heat removal. *Recommendation (for maintainer authorization — live config
  change): on the GPU path, either cap/disable `thermal_pressure` for ch1/ch5,
  or raise ch0's GPU response so the unobstructed exhaust works at least as hard
  as the radiator lanes. Validate against case-air-temperature, not duty.*

- **Dwell-dependence (C3) is a minor predictability cost,** inherent to the
  smootherstep-near-start ramp: a memjn parked at 65–66 °C makes the exhausts
  creep upward over tens of minutes with no temperature change. Upward-only, so
  thermally safe, but can read as "fans slowly getting louder for no reason."

Net: the GPU response is directionally right and the onset is fine; the one
behavior worth a maintainer decision is the 20-point `thermal_pressure` on the
radiator exhausts firing on GPU heat (F-DES1), which over-drives the restrictive
lanes relative to the free rear exhaust.

### 8.3 Follow-up retune (applied 2026-06-09)

The maintainer authorized a curve retune to reach higher duty sooner / more
gradually, hit half strength by ~75 C and strong by ~82 C, and bring the rear
exhaust ch0 up to parity with the radiator exhausts under medium+ load (while
keeping it low at low load for positive pressure). Applied to
`config/control.release.json` + `config/control.example.json` as **curve-only**
changes for ch0/ch1/ch5 and ch4 (>72 C); boosts and `cpu_override_curve`s
unchanged, so the CPU response and the time-gated build are preserved.
Validated: 21/21 contract tests, full local CI (122 pytest + build/CTest, exit
0). **Not** built/published/deployed. Details, before/after, and exact knots:
`docs/gpu-response-curve-retune-2026-06-09.md`.
