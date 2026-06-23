# Fan restart-restore behavior + plant model measurement — 2026-06-20

Read+write hardware measurement captured to de-risk a restart-based multi-profile
switch (the "restart that feels like hot-swap" idea). Captured on `snd-desk`
(9950X3D / RTX 5090 / ROG Strix X870-F / ProArt PA602), controller build
`505c2495969c`, at light load (Tctl ≈ 64 °C, GPU ≈ 48 °C). The live controller was
stopped for the window with its watchdog disabled, then restored; net change to the
running system is zero.

Raw artifacts (under `release/runtime/experiments/fan-plant-2026-06-20/`, not
committed):

- `plant_model_2026-06-20.json` — calibrate plant-model capture, schema
  `svg_mb_control.plant_model_capture.v1`.
- `snapshot_post_stop.json` — one-shot fan snapshot immediately after a graceful
  `--stop`.

## Finding 1 — a graceful worker stop hands fans back to BIOS SmartFan auto

On a graceful `--stop`, the worker restores every controlled channel to its
captured baseline, which rewrites the duty **and** the fan-mode register back to
BIOS SmartFan auto (`mode_raw` 0 → 0x40). This is implemented behavior, not a
guess (`src/control/control_loop.cpp:236-241` → `fan_sio.cpp:925-935`). Measured
duty/RPM before the stop (controller setpoints, `mode_raw` 0) vs immediately after
(BIOS auto, `mode_raw` 64):

| ch | role | controller (pre-stop) | BIOS auto (post-stop) | RPM Δ |
|----|------|-----------------------|-----------------------|-------|
| 0 | rear exhaust (140 iPPC) | 15.7 % → 608 rpm | 47.5 % → 1544 rpm | +936 (~2.5×) |
| 1 | radiator exhaust (140) | 22.7 % → 499 rpm | 36.5 % → 768 rpm | +269 |
| 2 | front intake (200) | 58.8 % → 707 rpm | 47.5 % → 612 rpm | −95 |
| 3 | front intake (200) | 54.9 % → 678 rpm | 47.5 % → 610 rpm | −68 |
| 4 | radiator intake (140 iPPC) | 33.7 % → 1165 rpm | 47.5 % → 1539 rpm | +374 |
| 5 | radiator exhaust (140) | 21.2 % → 778 rpm | 47.5 % → 1503 rpm | +725 (~1.9×) |

The pump (ch6) is `policy_blocked` and was untouched.

**Implication.** A profile switch implemented as a plain graceful worker restart is
**not** acoustically seamless: during the restart gap the fans revert to BIOS auto,
which at this operating point ran the rear exhaust and one radiator exhaust ~2×
faster — clearly audible. A switch is near-silent only at a quiet idle where the
controller setpoints already approximate BIOS auto. To make a restart-based switch
seamless under load, the switch path must **suppress restore and latch the last
controller duty** across the gap (skip the restore block at
`control_loop.cpp:222-276`). A latch then requires an out-of-process fan-safety
backstop: if restore is suppressed and the relaunch fails, fans freeze at the last
duty with no controller and no BIOS curve (`restore_on_exit=false`,
`sio_fan_writer.cpp:94`). This latched-no-fallback state already exists today only
on the crash/wedge force-restart path.

## Finding 2 — per-channel plant model (duty → steady RPM)

Calibrate sequence `0/20/40/60/80/100/0 %`, 7 s holds, 4 s settle window, abort
ceiling 90 °C. All six channels captured and restored cleanly (`restored=true`).
Steady RPM at each commanded duty:

| ch (fan) | 0 % | 20 % | 40 % | 60 % | 80 % | 100 % |
|----------|----:|-----:|-----:|-----:|-----:|------:|
| 0 rear exhaust (140 iPPC) | 164 | 801 | 1377 | 1922 | 2417 | 2869 |
| 1 radiator exhaust (140) | 164 | 427 | 820 | 1174 | 1512 | 1839 |
| 2 front intake (200) | 164 | 547 | 524 | 614 | 763 | 966 |
| 3 front intake (200) | 164 | 548 | 524 | 614 | 756 | 958 |
| 4 radiator intake (140 iPPC) | 164 | 792 | 1355 | 1874 | 2354 | 2787 |
| 5 radiator exhaust (140) | 164 | 766 | 1322 | 1835 | 2305 | 2733 |

Observations that generalize (the RPM numbers themselves do not — see caveats):

- The 200 mm intakes (ch2/ch3) are **non-monotonic at the low end** (20 % → ~547,
  40 % → ~524) and top out near 960 rpm; the 140 mm industrialPPC fans climb
  monotonically to ~2800 rpm. Duty→RPM is fan-specific and not linear, so control
  logic must stay in duty-% (PWM) space and treat RPM as observed telemetry — which
  the shipped controller already does.
- All channels read ~164 rpm at 0 % duty (the minimum-spin / tach floor for these
  fans on these headers); 0 % is not a guaranteed stop.

## Caveats

- The calibrate tool samples only the **settle window** at the end of each hold, so
  this is a **steady-state** duty→RPM map, not a transient slew/step-response curve.
  Per-channel slew timing was not captured.
- The RPM tachometer reacts slower than the fan's true speed change, so any
  RPM-derived transient timing would understate real responsiveness; tach-based slew
  timing is unreliable. This is a second reason the steady-state map is the reliable
  artifact and transient timing was not pursued here.
- These RPM values are specific to this machine's fans and headers and will differ
  on other rigs. They must not be hard-coded into general control or profile code.

## Drift

The duty→RPM map drifts over time (bearing wear, dust loading, fan aging) and
differs per machine. It is therefore **per-machine profile metadata**, not a
constant: it should be captured per rig and re-captured periodically to track drift,
with each capture timestamped. This is an input to the multi-machine profile design
(each machine carries its own captured plant snapshot for validation), not a value
the controller depends on at runtime.
