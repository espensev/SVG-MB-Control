# Normal Runtime Airflow Profile

Updated: 2026-05-27.

This note records the adopted low-load airflow profile and its validation
evidence. It is scoped to ordinary desktop/runtime operation, not short
high-power/high-temperature bursts.

The fan inventory, fan types, pressure strategy, floor philosophy, and
fan-relationship rules behind this profile live in
`docs\COOLING_STRATEGY.md`. The machine-readable policy reference is
`config\machines\snd-desk.cooling.policy.json`. This file keeps only the
adopted low-load settings and the evidence that confirms them.

The 2026-05-26 static-floor uplift remains the reference RPM observation.
The current release profile adopts lower hard intake minima plus
temperature-shaped low/medium curve points so the controller has real
low-band authority without holding channels `2`/`3`/`4` at high static
floors.

## Adopted Low-Load Settings

The adopted profile is recorded in `config\control.release.json`. The
packaged runtime copy is `release\control.json`. `config\control.example.json`
keeps the prior floors as a reference starting point for other hardware.

| Channel | Role | Reference static low-load duty | Current release min | Current low/medium curve |
|---:|---|---:|---:|---|
| 0 | Rear exhaust | 15.5% | 15.5% | unchanged rear-exhaust curve |
| 1 | Radiator exhaust A | 22% | 22% | CPU/radiator response preserved via `cpu_override_curve`; added GPU-airflow assist `curve` (gentle from ~60 C GPU, stronger toward ~80 C) |
| 2 | Front 200 mm intake | 60% | 42% | `35C:42%`, `50C:46%`, `62C:54%`, `72C:64%` |
| 3 | Front 200 mm intake | 56% | 38% | `35C:38%`, `50C:42%`, `62C:50%`, `72C:60%` |
| 4 | Front radiator Noctua intake | 31% | 24% | `35C:24%`, `50C:27%`, `62C:31%`, `72C:38%` |
| 5 | Radiator exhaust B | 20% | 20% | CPU/radiator response preserved via `cpu_override_curve`; added GPU-airflow assist `curve` (gentle from ~60 C GPU, stronger toward ~80 C) |

For channels `2`, `3`, and `4`, the same low/medium points are present in
both the primary `curve` and the `cpu_override_curve`. This preserves CPU
and GPU low-band response while removing the old high static intake floor.
The front 200 mm pair keeps the required `4%` spacing at every low/medium
point. See `docs\COOLING_STRATEGY.md` "Floor Philosophy" for the
heuristics.

## Pressure Model

The quick estimate uses the area/RPM proxy defined in the machine
policy:

```text
flow_index = rpm / 1000 * (fan_diameter_mm / 120)^2
```

This is not a real CFM measurement. It ignores radiator restriction, fan
curves, blade shape, and case impedance. It is sufficient for avoiding
the mistake of treating the 200 mm front fans and 140 mm radiator fans as
equivalent channels.

The reference low-load runtime before the dynamic low-end change, paired
with the previous static-floor observation captured from
`release\runtime\logs\svg_mb_control_output.csv` on 2026-05-26 at low
CPU/GPU load (Tctl ~46.75 C, GPU core ~28 C):

| Channel | Prior approx RPM | Current approx RPM |
|---:|---:|---:|
| 0 | 610 | 609 |
| 1 | 428 | 457 |
| 2 | 695 | 730 |
| 3 | 666 | 700 |
| 4 | 1003 | 1081 |
| 5 | 699 | 735 |

With radiator channels modeled as 140 mm fans, the earlier profile before
the static-floor uplift was close to neutral before radiator restriction.
The reference static-floor profile was intake-biased and remains the RPM
baseline used by policy tests until the dynamic profile is re-validated
live. The current curve shape should preserve the same direction by
keeping the large 200 mm intakes ahead of exhaust at idle/low load while
allowing lower idle duty than `60% / 56%`.

## Confirmation and Re-Validation Procedure

The reference static-floor profile was confirmed on the development host by
the steady-state runtime evidence above. At the time of the capture:

- per-channel `last_setpoint_pct` matched the then-configured floors within
  the PWM quantization step;
- `low_band_evidence.json` reported `activation_count = 0` and
  `max_debt < 1e-3` across the controlled channels, so the static floor
  uplift kept the integrated low-load signal below activation;
- `control_runtime.json` reported no open circuit breakers, no
  consecutive sensor or write failures, and `loop_slip_ms` under 1.2 ms
  against a 250 ms tick budget.

The same procedure must be re-run for the current dynamic low-end profile.
After the controller is intentionally restarted with the new config,
validate with
`release\runtime\logs\svg_mb_control_output.csv`:

- channels `2` and `3`, the PA602 stock 200 mm front intakes, should
  settle near the low/medium curve demand for the observed CPU/GPU
  temperatures, with channel `2` at least `4%` above channel `3`;
- channel `4`, the front radiator Noctua intake, should settle near its
  low/medium curve demand for the observed CPU/GPU temperatures;
- channels `1` and `5` should settle near their configured radiator
  floors;
- channel `0` should remain near its rear-exhaust floor;
- channel `6` should not be used in case pressure calculations.

If normal-runtime noise remains unacceptable, adjust the intake low/medium
curve points before raising or lowering exhaust floors. Any change must also
satisfy the tuning checklist in `docs\COOLING_STRATEGY.md` "How To Apply
During Tuning".

## References

- `docs\COOLING_STRATEGY.md` — strategy, fan inventory, floor
  philosophy, and fan-relationship rules.
- `config\machines\snd-desk.cooling.policy.json` — machine-readable
  policy.
- `docs\response-evaluation-tuning-plan.md` — pass design and
  acceptance criteria used to re-validate this profile.
