# Response Evaluation and Tuning Plan

## Data Baseline

Inputs used:

- local raw capture `alresponse.CSV`, 3243 parsed samples from the
  2026-05-12 response run; the raw CSV is intentionally kept out of git.
- Earlier `control-loop` CSV from
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_022043.csv`.
- CPU-response CSV from
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_023505.csv`.
- High-heat steady response CSV from
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_033423.csv`.
- Lower-heat/idle recovery CSV from
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_035931.csv`.
- Current live config in `config\control.release.json` and `release\control.json`.

Current live idle authority is good: Control holds channels `0-5` in manual mode
with setpoints `20,24,50,50,22,20`. Observed steady idle RPM is approximately:

| Channel | Role | Duty | RPM |
|---|---:|---:|---:|
| 0 | rear exhaust / case | 20% | 768 |
| 1 | radiator lane | 24% | 505 |
| 2 | front 200 mm | 50% | 644 |
| 3 | front 200 mm | 50% | 643 |
| 4 | radiator lane | 22% | 799 |
| 5 | radiator lane | 20% | 740 |
| 6 | blocked / not controlled | ~90% | ~2984 |

The old `208.5 ms` sample is historical. The current packaged response profile
uses a `50 ms` fixed-start-period control loop and a `50 ms` write cooldown.
The 2026-05-14 high-heat response run averaged `50.641 ms` achieved interval
over 19,432 rows, and the later lower-heat run averaged `50.610 ms` achieved
interval over 17,773 rows with no overrun rows.

From `alresponse.CSV`:

- Idle/low-power band: GPU power p50 `79 W`, GPU temp p50 `37.1 C`,
  GPU memory p50 `52 C`, CPU p50 `66.9 C`.
- GPU-load band: GPU power p50 `501 W`, GPU temp p50 `58 C`,
  GPU memory p50 `68 C`, GPU memory max `72 C`.
- CPU-heavy/low-GPU band: CPU package p50 `216 W`, CPU p50 `85.1 C`,
  while GPU memory stayed near `48 C`.

The key implication is that GPU airflow and CPU response are both required.
A pure GPU-envelope control strategy can keep GPU memory under control, but it
will not intentionally respond to Cinebench heat during a simultaneous max CUDA
run.

From the completed 2026-05-14 CPU response sample
`release\runtime\logs\archive\svg_mb_control_control-loop_20260514_023505.csv`:

- CPU Tctl was held in a tight band: p50 `85.25 C`, p90 `85.62 C`, max
  `86.25 C`.
- GPU memory was idle/cool: p50 `44 C`, max `48 C`.
- The original CPU overlay produced useful temperature control, but it also
  caused cyclic write reversals: about `44` up/down reversals per controlled
  channel while Tctl bounced between roughly `84.5-85.9 C`.
- Channels `1,4,5` received nearly identical CPU setpoints even though their
  RPM response differs, so the Noctua lanes should be staggered rather than
  moved as one synchronized block.

From the later 2026-05-14 steady response discovery
`release\runtime\logs\archive\svg_mb_control_control-loop_20260514_033423.csv`:

- CPU/Tctl max was `86.625 C`.
- Channel `1` reached `71.40%` setpoint with `22.00%` max thermal-pressure
  boost.
- Channel `4` reached `68.00%` setpoint with `20.00%` max boost.
- Channel `5` reached `65.86%` setpoint with `20.00%` max boost.
- The remaining downward candidate steps were small: channel `1` had `60` drops
  above `0.35%`, channel `4` had `52`, and channel `5` had none by that
  criterion.

The key implication is that the current controller is no longer missing steady
radiator authority. Further changes should be small, channel-specific, and
measured from comparable runs.

## Goals

1. Keep idle noise low by holding noisy radiator/case lanes near their current
   floors.
2. Keep the front 200 mm pair at a free-airflow baseline around `50%`.
3. During GPU load, prevent GPU memory junction from crossing the low `70 C`
   range for sustained periods.
4. During Cinebench plus max CUDA load, prevent CPU package/Tctl from sitting in
   the mid/high `80 C` range unless that is explicitly accepted as normal boost
   behavior.
5. Avoid control chatter: no repeated same-duty writes unless authority is lost
   or the setpoint actually changes.
6. Keep the response inside a tight envelope: do not keep raising airflow just
   because CPU temperatures can be pushed lower. Use the smallest curve that
   keeps CPU/GPU p90 and max inside the acceptance band.

## Evaluation Strategy

FanControl may keep owning the AIO path. For this controller, the test only
requires that FanControl does not write Control-owned channels `0-5`. Control
can reassert authority, but repeated authority loss on these channels invalidates
response data.

Use full CSV logging for every pass. For each run, record:

- workload start/stop wall-clock times
- ambient if available
- subjective noise notes at idle, ramp, steady load, cooldown
- `current_state.json`, `control_runtime.json`, active CSV, and event log

### Pass 1: Idle Hold

Duration: 10 minutes.

Acceptance:

- Channels `0-5` remain `mode_raw=0`.
- No `control_loop.authority_reasserted` events after startup.
- Duties remain within 1 raw PWM step of `20,24,50,50,22,20`.
- Subjective noise is acceptable at the desk.

If this fails, fix process ownership for channels `0-5` before touching curves.

### Pass 2: GPU Step Response

Run a GPU-heavy load with three stages:

1. idle baseline, 5 minutes
2. moderate GPU load around `250-350 W`, 5 minutes
3. high GPU load around `430-570 W`, 10-15 minutes
4. stop load and observe cooldown, 10 minutes

Primary metrics:

- time from GPU load start to first fan setpoint increase
- GPU memory p50/p90/max
- GPU core p50/p90/max
- fan RPM and duty p50/p90/max by channel
- cooldown time until GPU memory returns under `56 C`

Initial acceptance:

- GPU memory p90 under `70 C`.
- GPU memory max under `74 C`.
- Front 200 mm lanes ramp smoothly without oscillation.
- Radiator/case lanes do not jump audibly unless GPU memory enters the high
  `60 C` range.

### Pass 3: Combined CPU+GPU Response

Run Cinebench together with max CUDA load for 10-15 minutes after a 5 minute
idle baseline. This is the primary acceptance pass.

Primary metrics:

- CPU Tctl p50/p90/max
- CPU package power p50/p90
- GPU memory/core p50/p90/max
- per-channel RPM/duty p50/p90/max
- first duty increase time after load starts
- cooldown time until CPU Tctl and GPU memory return near baseline

Initial acceptance:

- CPU Tctl p90 below `88 C` and max below `92 C` after the initial transient.
- GPU memory p90 under `70 C` and max under `74 C`.
- Channels `0-5` remain `mode_raw=0`.
- No repeated authority reassertions after the initial writes.
- Ramp is audible but not abrupt; cooldown is slower than ramp-down chatter.
- If CPU Tctl is already held around `84-86 C` with acceptable clocks, do not
  raise curves further unless the combined-load max crosses the stop condition.

### Pass 4: CPU Isolation Diagnostic

Run a CPU-heavy workload with low GPU load only if the combined pass needs root
cause separation. This pass is diagnostic, not the gate for whether CPU response
exists; CPU response is already part of the shipped policy.

## Tuning Strategy

### Keep Current Idle Floors

Keep current floors unless noise notes say otherwise:

- channel `0`: `20%`
- channel `1`: `24%`
- channel `2`: `50%`
- channel `3`: `50%`
- channel `4`: `22%`
- channel `5`: `20%`

The front 200 mm pair should stay higher because their airflow is useful and
their noise cost is low. The rest should remain low at idle.

### GPU Response

Use GPU memory junction as the practical controlling signal through the current
GPU envelope. Based on the trace, `68-72 C` GPU memory is the important load
band.

Tuning direction:

- channels `2,3`: keep high baseline and let them lead GPU response
- channels `0,4,5`: add airflow later, only as GPU memory moves past the mid
  `60 C` range
- channel `1`: radiator lane can rise earlier than `0,4,5` if GPU load also
  warms coolant/case air

Do not lower front `2,3` below `50%` for the next pass.

### CPU Override

CPU response is mandatory. Each controlled channel now uses a separate
`cpu_override_curve` and commands `max(gpu_envelope_curve, cpu_override_curve)`.

- channels `1,4,5` get the strongest CPU response
- channel `0` gets a smaller rear-exhaust CPU assist
- channels `2,3` keep the 200 mm free-airflow baseline and add moderate CPU
  assist under high Tctl
- pre-rate-limit demand smoothing and a decay latch prevent small high-temp
  dips from causing immediate downwrites
- the radiator Noctua lanes `1,4,5` use staggered CPU breakpoints and decay
  rates; they should not have identical setpoints during CPU-only response

Candidate CPU override breakpoints for radiator lanes:

| CPU Tctl | Minimum radiator duty |
|---:|---:|
| 75 C | current floor |
| 82 C | 35% |
| 86 C | 50% |
| 90 C | 70% |
| 95 C | 90% |

This is implemented as an overlay, not by switching all channels back to
`max_cpu_gpu`.

Tight-envelope rule:

- If CPU p90 lands under `84 C` and noise is high, lower the `86 C` and `90 C`
  CPU override points by one small step.
- If CPU p90 lands in `84-88 C`, keep the curve unless noise notes demand a
  minor shape change.
- If CPU max exceeds `92 C` for more than a short transient, raise only the
  next-highest CPU override point that covers the excursion.
- Do not tune the front 200 mm pair upward for CPU alone unless radiator lanes
  are already near their target and CPU still exceeds the envelope.

## Implementation Plan

1. Add an offline response analyzer for Control native runtime manifests, CSV,
   and JSONL events. HWiNFO is not part of the normal logging workflow.
   Output idle/load/cooldown summaries, p50/p90/max, response delay, and
   authority-loss events.
2. Run the evaluation passes above and save the result summaries under
   `docs/` or a run-specific `runtime\analysis\` folder.
3. Keep CPU override support enabled while evaluating combined Cinebench plus
   max CUDA response.
4. Tune GPU curve breakpoints only after Pass 2 shows whether GPU memory p90/max
   exceeds the target band.
5. Rebuild with `.\build-release.ps1 -KeepBuildDir`, restart Control, and run a
   shorter verification pass after every curve change.

## Stop Conditions

Do not tune further in a run if:

- FanControl or another fan writer changes any Control-owned channel `0-5`.
- Any controlled channel leaves `mode_raw=0` repeatedly.
- GPU memory exceeds `76 C`.
- CPU Tctl exceeds `92 C` for more than a short transient.
- Control logs write failures, restore failures, or repeated authority
  reassertions.
