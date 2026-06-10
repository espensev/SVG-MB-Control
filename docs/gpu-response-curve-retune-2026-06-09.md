# GPU-Response Curve Retune — Exhausts + Radiator Intake (2026-06-09)

Status: **applied to config, not deployed** (no `build-release`/live restart).
Decision record for a tuning change to the GPU-driven fan response. Builds on
`docs/radiator-exhaust-gpu-response-decision-2026-06-09.md` (which added the
source-aware GPU response to CHA1/CHA5) and the review in
`docs/gpu-heat-response-chain-review-2026-06-09.md`.

## 1. Why

The review found the GPU response was concentrated in a narrow 64–76 °C band
and stayed low until ~90 °C: the radiator exhausts sat near their floor until
~66 °C and only reached ~49–53 % at 80 °C, while the *unobstructed* rear exhaust
(ch0) ran *lower* than the restrictive radiator exhausts under load (67 % vs
84–87 % at memjn 90 °C). The maintainer asked to:

- reach higher duty sooner / more gradually, not wait until 90 °C;
- hit ~half strength by **74–76 °C**, strong by **82 °C**, stronger **82→90 °C**;
- add **+5–10 pts near 72 °C**, kept slowly-built / time-gated;
- keep the rear exhaust (ch0) **low at low load** (positive pressure) but let it
  ramp under **medium+ load** to at least match the radiator exhausts;
- treat exact %s as a starting point to refine from `memjn` telemetry.

## 2. Decision

Reshape **only the per-channel GPU `curve`** for the three exhausts (ch0, ch1,
ch5) and the radiator-intake high end (ch4, knots > 72 °C only — its ≤72 °C
knots are locked to the machine policy soft-floor). **Leave all boosts and all
`cpu_override_curve`s unchanged.** Consequences:

- The **CPU response is identical** (CPU drives `cpu_override_curve` and the
  `observed_temp` boosts; none changed).
- The response stays **time-gated**: the unchanged `gpu_airflow` + `midband`
  boosts still ramp duty in over dwell, and `thermal_pressure` (unchanged,
  start 85.5 °C, max 20 % on ch1/ch5) carries the 86→90 °C "even stronger" push
  gradually rather than the curve slamming to 100 %.
- Front intakes ch2/ch3 are unchanged — they already reach 78/74 % by 72 °C.

## 3. Curves (temp_c : duty_pct)

Previous → new (only these four channels changed; floors and knot-locks kept):

```
ch0  old: 50:15.5 64:15.5 72:22 82:34 92:50 98:62
ch0  new: 50:15.5 64:15.5 68:22 72:36 76:48 80:58 84:66 88:72 92:78 98:86
ch1  old: 50:22 60:24 66:27 72:31 78:36 80:38 86:46 94:58 98:68
ch1  new: 50:22 60:24 64:27 68:34 72:42 76:49 80:55 84:59 88:61 94:63 98:65
ch5  old: 50:20 60:21 66:24 72:28 78:32 80:34 86:43 94:55 98:65
ch5  new: 50:20 60:21 64:24 68:31 72:39 76:46 80:52 84:56 88:58 94:60 98:62
ch4  old: 35:24 50:27 62:31 72:38 86:56 94:76 98:88
ch4  new: 35:24 50:27 62:31 72:38 78:50 84:60 90:70 96:78   (<=72 locked to policy)
```

Contract guards verified: ch1 ≥ ch5 + 2 at every shared knot
(`test_radiator_exhaust_gpu_curves_are_staggered_not_mirrored`); ch4's first
four knots equal the policy soft-floor
(`test_release_intake_low_end_curves_follow_machine_policy`); all curves
monotonic; idle floors unchanged (ch0 15.5, ch1 22, ch5 20, ch4 24).

## 4. Simulated effect (CPU 40 °C, sustained duty %)

| GPU °C | ch0 old→new | ch1 old→new | ch5 old→new | ch4 old→new |
|---:|---|---|---|---|
| ≤64 (idle) | 15.5 → 15.5 | 26 → 27 | 23 → 24 | 31 → 31 |
| 72 | 34 → 48 | 46 → 57 | 43 → 54 | 53 → 53 |
| 75–76 | 36 → 59 | 49 → 63 | 45 → 60 | 56 → 62 |
| 80 | 45 → 70 | 53 → 70 | 49 → 67 | 64 → 67 |
| 82 | 46 → 74 | 55 → 72 | 51 → 69 | 68 → 73 |
| 86 | 51 → 81 | 64 → 78 | 61 → 75 | 85 → 91 |
| 90 | 67 → 93 | 87 → 96 | 84 → 93 | 95 → 99 |

Rear exhaust ch0 now meets or exceeds the radiator exhausts under load (74 % vs
72/69 at 82 °C; 93 % at 90 °C) while staying at its 15.5 % floor ≤64 °C. The
10-min vs 60-min hold still differ in the upper band (e.g. ch1 at 86 °C: 75.5 %
at 10 min, 78.2 % at 60 min), confirming the response remains time-gated.

Source of the simulation: faithful tick-by-tick port of the shipped control
pipeline (`tools/gpu_response_sim.py` / `gpu_response_tuning.py`, throwaway,
not committed), validated against `tests/cpp/boost_stage_tests.cpp` (diff 0.0)
and hand/independent recompute (±0.3 pt). Duty is not RPM/airflow; >80 °C
`memjn` is rare on this build; exact %s should be refined from runtime CSV.

## 5. Validation

- `tests/test_config_contracts.py` + `tests/test_machine_cooling_policy.py`:
  21/21 pass with the applied curves.
- Full local CI (`scripts/Test-LocalCI.ps1 -KeepBuildDir`): [recorded in the
  session; build + CTest + pytest].
- Applied to `config/control.release.json` and `config/control.example.json`
  (curve points only; every other field byte-identical).
- Deployed live 2026-06-09: the published `release/control.json` (18,550 bytes,
  sha256 `b51e542a...726616`) carries these curves and is byte-identical to
  `config/control.release.json`, and the running worker's CSV session header
  records the same `config_sha256`, so the live controller loaded the retuned
  config (verified read-only 2026-06-10). The live executable stamps
  `git_hash=c4b6986` because the config was published before the retune commit
  `767a901` existed — a provenance-stamp mismatch only; the retune is config
  data, with no code change involved.

## 6. Follow-ups

- Capture a real GPU-load CSV (logging `core`, `memjn`, `hotspot` separately)
  and refine the exact knot %s against it; confirm the time-gated build and the
  ch0-vs-ch1/ch5 ordering on hardware.
- Open question from the review (whether `thermal_pressure`'s 20-pt GPU
  contribution on ch1/ch5 should be capped) — **resolved 2026-06-10: (C) accept +
  document** as case-pressure headroom; it stays preserved so the CPU response is
  untouched. See `docs/gpu-heat-response-chain-review-2026-06-09.md` F-DES1 and
  `docs/COOLING_STRATEGY.md` (Radiator Exhaust Pair).
