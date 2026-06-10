# Decision: radiator-exhaust GPU-airflow response (channels 1, 5) — 2026-06-09

**Status:** Accepted and deployed live 2026-06-09 ~15:06 (config tuning of
existing mechanisms; no code change). See **Deployment** below.
**Scope:** `config/control.release.json` + `config/control.example.json`
channels 1 and 5; companion doc/test updates. Config is read at controller
startup (not hot-reloaded); deployment was effected by `build-release.ps1` +
the watchdog scheduled task relaunching the worker.

## Problem

Analysis of the 2026-06-09 07:06 GPU excursion (memory junction reached 78 C;
`docs`/runtime analysis under `release/runtime/analysis/gpu-fan-response-20260609/`)
showed the two front 200 mm case intakes (CHA2/CHA3) ramp hard on GPU heat
(+20 pts of duty, to ~85 %/81 %). The two radiator **exhausts** (CHA1/CHA5) were
`temp_blend = cpu_only`, so they reacted to that GPU heat only by ~+5 pts via the
flat-ceiling `gpu_airflow` boost. This change reconfigures the radiator
exhausts to ramp **with GPU temperature** — gentle from ~60 C, stronger toward
~80 C — via the `max_cpu_gpu_source_aware` blend, so their setpoint follows GPU
temperature rather than holding at the flat-ceiling boost. The response keys off
the GPU temperature input regardless of whether a measured temperature falls.

## Why not the `gpu_airflow` boost alone

`gpu_airflow` (boost_stage.cpp) is a flat-ceiling time-integrator: GPU
temperature sets the ramp **rate**, not the ceiling. At any sustained GPU above
its `start_c` it saturates at `max_boost_pct` and holds, so 72 C and 78 C command
the **same** steady exhaust duty. That delivers "kick in once hot," not the
requested "more at 80 than at 70." A temperature-**proportional** response needs
a GPU curve.

## Decision

Switch CHA1/CHA5 to `temp_blend = max_cpu_gpu_source_aware`
(`source_aware_cpu_hot_guard_c = 75.0`, matching CHA0/CHA2/CHA3/CHA4) and:

- **`curve`** = a GPU-airflow assist curve, flat at the floor below ~60 C,
  gentle 60–70 C, steep 70–80 C, rising to its top knots (see the table below).
  Applied to the GPU envelope while CPU < 75 C (the source-aware primary input).
- **`cpu_override_curve`** = the elementwise maximum of the prior CPU `curve`
  and the prior CPU-emergency override. Because the evaluator takes
  `max(curve(primary), cpu_override_curve(CPU))` every tick and sets
  `observed_temp_c` to whichever wins, the full CPU/radiator exhaust response —
  including the `midband`/`thermal_pressure` overlays, which key off
  `observed_temp_c` — is preserved when CPU is the driving signal.
- The GPU `curve`s are **staggered, not mirrored**, to honour
  `radiator_exhaust_pair.allow_mirrored_response = false`: CHA1 and CHA5 share
  the same temperature knots and CHA1 stays strictly above CHA5 by >= 2 pts at
  every knot (no crossing), so two same-model NF-A14s on one radiator do not
  arrive at the same duty/RPM at the same GPU temperature. This is enforced by
  `tests/test_config_contracts.py::test_radiator_exhaust_gpu_curves_are_staggered_not_mirrored`
  (the analogue of the intake pair's >= 4 pt spacing guard). On the **CPU**-driven
  path the two `cpu_override_curve`s remain near-identical, as before this change;
  CPU-side separation continues to rest on floor (`22 %` vs `20 %`) and
  `decay_latch_above_pct` (`38 %` vs `32 %`) — unchanged here.

Release GPU `curve` (duty raw, before boosts; CHA1 leads CHA5 by >= 2 at every knot):

| GPU C | 50 | 60 | 66 | 72 | 78 | 80 | 86 | 94 | 98 |
|---|---|---|---|---|---|---|---|---|---|
| ch1 | 22 | 24 | 27 | 31 | 36 | 38 | 46 | 58 | 68 |
| ch5 | 20 | 21 | 24 | 28 | 32 | 34 | 43 | 55 | 65 |

> **Note — curve retuned later the same day (not redeployed).** The table above
> is the GPU `curve` **deployed at 15:06** (what the live worker runs). The
> `curve`s were subsequently retuned (steeper mid-band, higher top knots) in
> `config/control.{release,example}.json`, described in
> `docs/gpu-response-curve-retune-2026-06-09.md` and `docs/COOLING_STRATEGY.md`.
> That retune is in config but has not been rebuilt or redeployed, so the table
> and the replay/side-effect figures below still describe the live curve.

## Simulated replay of the recorded 06:30–07:30 trace

Simulated raw setpoint + the integrated boosts against the recorded
`gpu_memjn_c`/`cpu_tctl_c` (curve_shape quintic smootherstep, matching
`LookupCurve`):

- Idle (GPU 40–50, CPU 60–67): exhaust delta ≈ 0 — preserved.
- CPU-96 emergency (06:47): 99.8 % → 100 % — CPU response preserved.
- GPU-driven 07:00–07:06: exhaust total ramps ~31 % (GPU 66) → ~43–46 % (GPU 72)
  → **CHA5 ~47 % / CHA1 ~51 % at GPU 76–78** — duty rises with GPU temperature
  (more at 76 than 72 than 66), with CHA1 above CHA5 by ~4 pts at the peak (the
  configured stagger).
- Post-peak: boost overlays bleed off over ~minutes at the configured fall rate.

This is a **replay against the recorded trace; it has not been checked on live
hardware.** The first live GPU excursion after deployment can be checked against
this simulated expectation — including listening for any audible beat between
CHA1 and CHA5 (the curve stagger keeps CHA1 >= 2 pts above CHA5 at every knot).

## Deployment (2026-06-09 ~15:06)

Deployed live with operator authorization. `build-release.ps1` ran clean
(exit 0): build 122/122, CTest 11/11, 122 Python tests, published
`release/control.json` (17,167 bytes), archived
`svg-mb-control-20260609-1506.zip`. The "SVG-MB Control" watchdog scheduled
task relaunched the worker at 15:06:42; `control_runtime.json` reported status
`running` with the tick advancing.

Live readings: CHA1/CHA5 now report `primary_temp_source = gpu` (was
`cpu`), so the source-aware blend is in effect; at idle GPU the
`cpu_override` path carried the CPU response
(`response = cpu_override`), so the CPU exhaust behaviour is unchanged.
The GPU-driven ramp has been **replayed against the recorded trace but not
checked on live hardware** — the first real GPU excursion after deployment can
be checked against the ramp in **Simulated replay** (CHA5 ~47 % / CHA1 ~51 % at
GPU 78) and for any audible CHA1/CHA5 beat.

## Known side effect (accepted)

In the CPU 78–86 C guard zone with GPU cool, the source-aware path applies the
(now steeper) GPU `curve` to `max(CPU, GPU)`, so the exhausts run ~+10 pts higher
than before on high CPU-only load. The CPU-emergency override still dominates
above ~88 C.

## Apply / rollback

- Apply: done — see **Deployment**. (Config is read at startup, not
  hot-reloaded; reapplying requires a rebuild/publish + worker relaunch.)
- Rollback: revert the two config channels (and this record/test/doc set) and
  re-run `build-release.ps1`; no code or schema change is involved.
