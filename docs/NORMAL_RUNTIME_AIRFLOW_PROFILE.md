# Normal Runtime Airflow Profile

Updated: 2026-05-25

This note records the low-load airflow assumptions behind the normal-runtime
fan profile. It is scoped to ordinary desktop/runtime operation, not short
high-power/high-temperature bursts.

## Scope

- Count the six airflow fans controlled by this repo's policy: channels 0-5.
- Exclude channel 6 from case pressure math. It is AIO/pump scope and is
  controlled separately.
- Prefer positive case pressure during low load and normal runtime.
- Do not raise rear exhaust for this goal; use the quiet front 200 mm intakes
  to preserve intake margin while modestly increasing radiator airflow.

## Hardware Basis

The shared hardware docs live outside this repo:

- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\Fan-type-placement.md`
- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\hardware.md`
- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\noctua_nf_a14_industrialPCC_3000_pwm_infosheet_en.pdf`
- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\noctua_nf_a14_industrialPCC_2000_pwm_infosheet_en.pdf`
- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\Q27037_ProArt_PA602_WW_UM_V5_WEB.pdf`

Relevant hardware facts:

| Fan group | Model / source | Direction | Notes |
|---|---|---|---|
| Front case intake | 2 x ASUS ProArt PA602 stock 200 x 38 mm PWM fans | Intake | Quiet, high-flow front case fans, fan hub bypassed |
| Radiator fans | Noctua NF-A14 industrialPPC 3000 PWM / 2000 PWM, 140 mm | Mixed radiator airflow | Use 140 mm fan math, not 120 mm |
| Rear case fan | Noctua NF-A14 industrialPPC 3000 PWM, 140 mm | Exhaust | Keep low-load floor unchanged |
| AIO / channel 6 | AIO/pump scope | Excluded | Do not include in case pressure ratios |

## Adopted Low-Load Settings

The adopted profile is recorded in `config\control.release.json`. The packaged
runtime copy is `release\control.json`. `config\control.example.json` keeps the
prior floors as a reference starting point for other hardware.

| Channel | Role | Prior floor | Current floor |
|---:|---|---:|---:|
| 0 | Rear exhaust | 15.5% | 15.5% |
| 1 | Radiator / slow Noctua | 20% | 22% |
| 2 | Front 200 mm intake | 54% | 60% |
| 3 | Front 200 mm intake | 50% | 56% |
| 4 | Front radiator Noctua | 28% | 31% |
| 5 | Mid radiator Noctua | 18% | 20% |

Matching low-temperature curve points and low CPU override plateaus were raised
with the same intent, so the floors are not contradicted by the first curve
segments.

## Pressure Model

The quick estimate uses an area/RPM proxy:

```text
flow_index = rpm / 1000 * (fan_diameter_mm / 120)^2
```

This is not a real CFM measurement. It ignores radiator restriction, fan curves,
blade shape, and case impedance. It is still useful for avoiding the mistake of
treating the 200 mm front fans and 140 mm radiator fans as identical channels.

Observed low-load runtime before the change showed the controlled channels near:

| Channel | Approx observed low-load RPM |
|---:|---:|
| 0 | 610 |
| 1 | 428 |
| 2 | 695 |
| 3 | 666 |
| 4 | 1003 |
| 5 | 699 |

With radiator channels modeled as 140 mm fans, the prior low-load profile was
close to neutral before radiator restriction. The adopted profile intentionally
raises the front 200 mm intake floors more than the radiator floors to keep the
low-load profile intake-biased.

## Re-Validation Check After Re-Tuning

Initial validation on the development host confirmed acceptable noise at the
adopted floors. The same procedure applies if the profile is re-tuned: after
the controller is intentionally restarted with the new config, validate with
`release\runtime\logs\svg_mb_control_output.csv`:

- channels 2 and 3 should settle near the configured front-intake floors at
  low load;
- channels 1, 4, and 5 should settle near their configured radiator floors;
- channel 0 should remain near its rear-exhaust floor;
- channel 6 should not be used in case pressure calculations.

If normal-runtime noise becomes unacceptable, reduce radiator floors first or
back off only the front 200 mm intake floors.

## References

- ASUS ProArt PA602 product page: <https://www.asus.com/us/motherboards-components/cases/proart/proart-pa602/>
- ASUS PA602 press release, including front 200 mm fan airflow note: <https://press.asus.com/news/press-releases/asus-proart-pa602-chassis/>
- Noctua NF-A14 industrialPPC-3000 PWM specifications: <https://www.noctua.at/en/products/nf-a14-industrialppc-3000-pwm/specification>
- Noctua NF-A14 industrialPPC-2000 PWM specifications: <https://www.noctua.at/en/products/nf-a14-industrialppc-2000-pwm/specifications>
