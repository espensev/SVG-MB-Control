# Response Evaluation and Tuning Plan

Status: current as of 2026-05-26. Maintained alongside
`docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` (adopted airflow profile rationale)
and `docs\CONTROL_PIPELINE_MATH.md` §13 (real-data validation history).
This document records the standing evaluation methodology — pass design,
acceptance criteria, and tuning strategy — against the **currently shipped**
control profile. Historical 50 ms-era evidence is kept in
`docs\CONTROL_PIPELINE_MATH.md` §13.1 and the `discovery-*.md` files; do
not treat that older evidence as a description of current shipped behavior.

## Currently Shipped Profile

Reference baseline for every pass (from `config\control.release.json` and
`release\control.json`, schema_version 4):

- Control cadence: `poll_tick_ms = 250`, `write_cooldown_ms = 250`.
- Deadband: `deadband_pct = 0.25`.
- Live channels: `0, 1, 2, 3, 4, 5`. Channel `6` is blocked by live policy.
- Adopted low-load floors (channels `0/1/2/3/4/5`):
  `15.5% / 22% / 60% / 56% / 31% / 20%`. See
  `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` for the hardware rationale and
  for the ch2-ch3 ≥ 4% resonance guard in
  `tests\test_config_contracts.py::test_shipped_control_loop_configs_use_smooth_step_cadence`.
- Authority bias for high-CPU response:
  `thermal_pressure_max_boost_pct = 20.0` on channels `1` and `5`
  (`cpu_only` radiator lanes) versus `14.0` on channel `4`
  (`max_cpu_gpu` front radiator Noctua). CPU override curves jump
  aggressively on channels `1` and `5` at `88-92 C` while channel `4`
  climbs more gradually.

## Recent Validation Evidence

- **2026-05-24 Cinebench R23 (pre-adoption)**: CPU-heavy segment held
  CPU/Tctl at p50 `87.625 C`, p90 `88.25 C`, max `89.25 C`; GPU memory
  stayed cool (p90 `48 C`, max `50 C`). The release config was
  subsequently adjusted to shift high-CPU authority toward channels `1`
  and `5` and reduce channel `4` high-heat dominance. Full record in
  `docs\CONTROL_PIPELINE_MATH.md` §13.1 (config sha256
  `51a16ea6…fbc78`, git hash `94a1d4c6a34c`).
- **2026-05-26 idle steady-state (post-adoption)**: confirmation of the
  elevated floors. Per-channel `last_setpoint_pct` matched the configured
  floors within the PWM quantization step
  (`15.5 / 22.0 / 60.15 / 56.15 / 31.0 / 20.0`%);
  `low_band_evidence.json` reported `activation_count = 0` and
  `max_debt < 1e-3`; `loop_slip_ms ≤ ~1.1 ms` against the 250 ms tick
  budget; no circuit-breaker opens, sensor failures, or write failures.
  Full record in `docs\CONTROL_PIPELINE_MATH.md` §13.1 (config sha256
  `036cda22…3f06`, git hash `b396b53a94a9`).

The outstanding measurement is a Cinebench + max-CUDA combined pass
against the post-adoption config to verify the new CPU + GPU response
together.

## Goals

1. Hold idle noise low by keeping the radiator/case lanes near the adopted
   floors and respecting the documented ch2-ch3 resonance guard.
2. Keep the front 200 mm pair (channels `2`, `3`) at the adopted
   intake-bias floors (`60%` / `56%`). The front airflow is useful and the
   noise cost at the desk is low.
3. During GPU load, prevent the GPU memory junction from sustaining above
   the low `70 C` range.
4. During Cinebench together with max CUDA load, prevent CPU Tctl from
   sitting in the mid-to-high `80 C` range unless that is explicitly
   accepted as normal boost behavior.
5. Avoid control chatter: no repeated same-duty writes unless authority is
   lost or the setpoint actually changes (gated by deadband, cooldown, and
   the authority-reassert path in `CONTROL_PIPELINE_MATH.md` §9).
6. Keep the response inside a tight envelope: do not raise airflow further
   just because CPU temperatures can be pushed lower. Use the smallest
   curve that keeps CPU/GPU p90 and max inside the acceptance band.

## Evaluation Strategy

FanControl may keep owning the AIO path. For this controller, the test
only requires that FanControl does not write Control-owned channels
`0-5`. Control can reassert authority, but repeated authority loss on
these channels invalidates response data.

Use full CSV logging for every pass. For each run, record:

- workload start / stop wall-clock times,
- ambient temperature if available,
- subjective noise notes at idle, ramp, steady load, and cooldown,
- `current_state.json`, `control_runtime.json`, the active CSV, the
  event log, and the manifest config sha256 / git hash.

### Pass 1: Idle Hold

Duration: 10 minutes.

Acceptance:

- Channels `0-5` remain `mode_raw = 0`.
- No `control_loop.authority_reasserted` events after startup.
- Per-channel `last_setpoint_pct` stays within one raw PWM step of
  `15.5%, 22%, 60.15%, 56.15%, 31%, 20%`. The `60.15%` / `56.15%`
  figures account for the configured low-temperature curve point at
  `58 C` lifting channels `2` and `3` slightly above the floor at idle
  Tctl, then held by the decay latch — see
  `CONTROL_PIPELINE_MATH.md` §4 and §5.
- `low_band_evidence.json` shows `activation_count = 0` and
  `max_debt < 1e-3` over the window.
- Subjective noise is acceptable at the desk.

If this fails, fix process ownership for channels `0-5` before touching
curves.

### Pass 2: GPU Step Response

Run a GPU-heavy load with stages:

1. idle baseline, 5 minutes,
2. moderate GPU load around `250-350 W`, 5 minutes,
3. high GPU load around `430-570 W`, 10-15 minutes,
4. stop load and observe cooldown, 10 minutes.

Primary metrics:

- time from GPU load start to first fan setpoint increase,
- GPU memory p50 / p90 / max,
- GPU core p50 / p90 / max,
- fan RPM and duty p50 / p90 / max by channel,
- cooldown time until GPU memory returns under `56 C`.

Initial acceptance:

- GPU memory p90 under `70 C`.
- GPU memory max under `74 C`.
- Front 200 mm lanes ramp smoothly without oscillation.
- Radiator / case lanes do not jump audibly unless GPU memory enters the
  high `60 C` range.

### Pass 3: Combined CPU + GPU Response

Run Cinebench together with max CUDA load for 10-15 minutes after a
5 minute idle baseline. This is the primary acceptance pass.

Primary metrics:

- CPU Tctl p50 / p90 / max,
- CPU package power p50 / p90,
- GPU memory / core p50 / p90 / max,
- per-channel RPM / duty p50 / p90 / max,
- first duty increase time after load starts,
- cooldown time until CPU Tctl and GPU memory return near baseline.

Initial acceptance:

- CPU Tctl p90 below `88 C` and max below `92 C` after the initial
  transient.
- GPU memory p90 under `70 C` and max under `74 C`.
- Channels `0-5` remain `mode_raw = 0`.
- No repeated authority reassertions after the initial writes.
- Ramp is audible but not abrupt; cooldown is slower than ramp-down
  chatter.
- If CPU Tctl is already held around `84-86 C` with acceptable clocks,
  do not raise curves further unless the combined-load max crosses the
  stop condition.

### Pass 4: CPU Isolation Diagnostic

Run a CPU-heavy workload with low GPU load only if the combined pass
needs root-cause separation. This pass is diagnostic, not the gate for
whether CPU response exists; CPU response is part of the shipped policy.

## Tuning Strategy

### Keep Current Idle Floors

The adopted floors are recorded in `config\control.release.json`. Do not
lower any of them unless noise notes and Pass 1 / 2 / 3 evidence justify
the change:

- channel `0` (rear exhaust): `15.5%`
- channel `1` (radiator Noctua, `cpu_only`): `22%`
- channel `2` (front 200 mm intake): `60%`
- channel `3` (front 200 mm intake): `56%`
- channel `4` (front radiator Noctua, `max_cpu_gpu`): `31%`
- channel `5` (mid radiator Noctua, `cpu_only`): `20%`

The front 200 mm pair stays higher because their airflow is useful and
the noise cost is low. The `≥ 4%` spacing between channels `2` and `3`
is enforced by `test_shipped_control_loop_configs_use_smooth_step_cadence`
and avoids same-rpm resonance between the two front fans.

### GPU Response

Use the GPU memory junction as the practical controlling signal through
the configured GPU envelope (`max(core, memjn, hotspot when > 0)`, per
`CONTROL_PIPELINE_MATH.md` §3.2). The historical 2026-05-12 trace
identified `68-72 C` GPU memory as the important load band.

Tuning direction:

- channels `2`, `3`: keep the elevated intake floor and let them lead
  GPU response.
- channels `0`, `4`, `5`: add airflow later, only as GPU memory moves
  past the mid `60 C` range.
- channel `1`: radiator lane can rise earlier than `0`, `4`, `5` if
  GPU load also warms coolant or case air.

Do not lower channels `2` or `3` below their adopted floors for the
next pass.

### CPU Override

CPU response is mandatory. Each controlled channel uses a separate
`cpu_override_curve` and the per-channel demand is
`max(primary_curve, cpu_override_curve)` (see
`CONTROL_PIPELINE_MATH.md` §4.4). Currently shipped behavior:

- channels `1`, `5` carry the strongest CPU response — `cpu_only`
  blend, `thermal_pressure_max_boost_pct = 20.0`, override curve jumps
  from the floor to `58%` at `88 C`, `74%` at `90 C`, `88%` at `92 C`,
  and `100%` at `96 C`.
- channel `0` carries a smaller rear-exhaust CPU assist — override
  climbs from `15.5%` floor to `18%` at `84 C`, `28%` at `88 C`, `42%`
  at `92 C`, and `58%` at `96 C`.
- channel `4` (front radiator Noctua, `max_cpu_gpu`) carries the
  secondary radiator authority — `thermal_pressure_max_boost_pct = 14.0`,
  override climbs more gradually (`32%` at `82 C`, `42%` at `86 C`,
  `54%` at `90 C`, `70%` at `95 C`). This is the deliberate "shift
  authority toward channels `1` and `5`, reduce channel `4` high-heat
  dominance" outcome recorded in `CONTROL_PIPELINE_MATH.md` §13.1.
- channels `2`, `3` keep the 200 mm free-airflow baseline and add
  moderate CPU assist under high Tctl (override climbs from the floor
  at `75 C` to `86%` at `95 C` on channel `2`, `82%` at `95 C` on
  channel `3`).
- pre-rate-limit demand smoothing and a decay latch prevent small
  high-temp dips from causing immediate downwrites (see
  `CONTROL_PIPELINE_MATH.md` §5).

Tight-envelope rule:

- If CPU p90 lands under `84 C` and noise is high, lower the `86 C`
  and `90 C` CPU override points by one small step on the lane that
  dominated noise.
- If CPU p90 lands in `84-88 C`, keep the curve unless noise notes
  demand a minor shape change.
- If CPU max exceeds `92 C` for more than a short transient, raise
  only the next-highest CPU override point that covers the excursion.
- Do not tune the front 200 mm pair upward for CPU alone unless
  radiator lanes are already near their target and CPU still exceeds
  the envelope.

## Implementation Plan

1. Use the in-tree analyzer (`svg-mb-control.exe --analyze ...`) to
   ingest each run's CSV and JSONL events and emit p50 / p90 / max
   summaries, response-delay metrics, and authority-loss events. See
   `docs\RUNTIME_LOGGING_AND_EVALUATION.md` for the analyzer workflow.
2. Save the decision record produced by `--analyze` next to the manifest
   under `release\runtime\` or a per-run `runtime\analysis\` folder.
3. Keep CPU override enabled while evaluating combined Cinebench +
   max CUDA response.
4. Tune GPU curve breakpoints only after Pass 2 shows whether GPU
   memory p90 / max exceeds the target band.
5. Rebuild with `.\build-release.ps1`, restart Control, and run a
   shorter verification pass after every curve change.

## Stop Conditions

Do not tune further in a run if:

- FanControl or another fan writer changes any Control-owned channel
  `0-5`.
- Any controlled channel leaves `mode_raw = 0` repeatedly.
- GPU memory exceeds `76 C`.
- CPU Tctl exceeds `92 C` for more than a short transient.
- Control logs write failures, restore failures, or repeated authority
  reassertions.
