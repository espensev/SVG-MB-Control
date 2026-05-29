# Response Evaluation and Tuning Plan

Status: current as of 2026-05-27. Maintained alongside
`docs\COOLING_STRATEGY.md` (strategy, fan inventory, floor philosophy,
fan-relationship rules),
`docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` (adopted airflow profile and
validation evidence), and `docs\CONTROL_PIPELINE_MATH.md` §13 (real-data
validation history).
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
- Release hard minima (channels `0/1/2/3/4/5`):
  `15.5% / 22% / 42% / 38% / 24% / 20%`. Intake channels `2`, `3`,
  and `4` then use dynamic low/medium points through `72 C` instead of
  the previous static `60% / 56% / 31%` low-load floor. See
  `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` and
  `tests\test_config_contracts.py::test_release_intake_low_end_curves_follow_machine_policy`.
- Machine-level fan topology and cooling intent are captured in
  `docs\COOLING_STRATEGY.md` and
  `config\machines\snd-desk.cooling.policy.json`; this plan applies
  those roles rather than redefining them.
- Authority bias for high-CPU response:
  `thermal_pressure_max_boost_pct = 20.0` on channels `1` and `5`
  (`cpu_only` radiator lanes) versus `14.0` on channel `4`
  (`max_cpu_gpu` front radiator Noctua intake). CPU override curves jump
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
- **2026-05-26 idle steady-state (static-floor reference)**: confirmation of
  the previous elevated-floor profile. Per-channel `last_setpoint_pct`
  matched the then-configured floors within the PWM quantization step
  (`15.5 / 22.0 / 60.15 / 56.15 / 31.0 / 20.0`%);
  `low_band_evidence.json` reported `activation_count = 0` and
  `max_debt < 1e-3`; `loop_slip_ms ≤ ~1.1 ms` against the 250 ms tick
  budget; no circuit-breaker opens, sensor failures, or write failures.
  Full record in `docs\CONTROL_PIPELINE_MATH.md` §13.1 (config sha256
  `036cda22…3f06`, git hash `b396b53a94a9`).

Outstanding measurements:

- an idle hold against the dynamic low-end profile to record replacement
  RPMs and subjective noise;
- a Cinebench + max-CUDA combined pass against the current config to verify
  the CPU + GPU response together.

## Goals

1. Hold idle noise low by keeping hard minima modest and letting the
   low/medium curve points, not high static floors, provide pressure bias.
2. Keep the PA602 stock front 200 mm intake pair (channels `2`, `3`) spaced
   by at least `4%` through the low/medium curve. The front airflow is useful,
   but it should now move with demand instead of staying pinned at
   `60% / 56%`.
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
- Per-channel `last_setpoint_pct` follows the configured curve demand for
  the observed CPU/GPU temperatures. At the prior idle reference point
  (Tctl ~46.75 C, GPU core ~28 C), the expected intake demands are roughly
  channel `2` `45.7%`, channel `3` `41.7%`, and channel `4` `26.8%`
  before PWM quantization and rate-limit settling.
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

### Keep Current Low-End Policy

The release minima and low/medium intake curves are recorded in
`config\control.release.json`. Do not raise the hard intake minima back to
the old static profile unless Pass 1 / 2 / 3 evidence proves the dynamic
curve cannot maintain pressure bias:

- channel `0` (rear exhaust): `15.5%`
- channel `1` (radiator Noctua, `cpu_only`): `22%`
- channel `2` (PA602 stock front 200 mm intake): min `42%`, then
  `35C:42%`, `50C:46%`, `62C:54%`, `72C:64%`
- channel `3` (PA602 stock front 200 mm intake): min `38%`, then
  `35C:38%`, `50C:42%`, `62C:50%`, `72C:60%`
- channel `4` (front radiator Noctua intake, `max_cpu_gpu`): min `24%`,
  then `35C:24%`, `50C:27%`, `62C:31%`, `72C:38%`
- channel `5` (mid radiator Noctua, `cpu_only`): `20%`

The front 200 mm pair keeps `≥ 4%` spacing between channels `2` and `3`
at every low/medium point. This is enforced by
`test_release_intake_low_end_curves_follow_machine_policy` and avoids
same-rpm resonance between the two front fans.

### GPU Response

Use the GPU memory junction as the practical controlling signal through
the configured GPU envelope (`max(core, memjn, hotspot when > 0)`, per
`CONTROL_PIPELINE_MATH.md` §3.2). The historical 2026-05-12 trace
identified `68-72 C` GPU memory as the important load band.

Tuning direction:

- channels `2`, `3`: keep the dynamic low/medium intake curve and let them
  lead GPU response.
- channels `0`, `4`, `5`: add airflow later, only as GPU memory moves
  past the mid `60 C` range.
- channel `1`: radiator lane can rise earlier than `0`, `4`, `5` if
  GPU load also warms coolant or case air.

Do not collapse channels `2` or `3` back into a shared curve or remove
their `4%` low-end spacing for the next pass.

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
- channel `4` (front radiator Noctua intake, `max_cpu_gpu`) carries the
  secondary radiator authority — `thermal_pressure_max_boost_pct = 14.0`,
  override climbs more gradually (`32%` at `82 C`, `42%` at `86 C`,
  `54%` at `90 C`, `70%` at `95 C`). This is the deliberate "shift
  authority toward channels `1` and `5`, reduce channel `4` high-heat
  dominance" outcome recorded in `CONTROL_PIPELINE_MATH.md` §13.1.
- channels `2`, `3` keep the 200 mm front-intake free-airflow baseline and add
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

1. Use the in-tree analyzer (`svg-mb-control.exe analyze ingest` followed by
   `svg-mb-control.exe analyze report`) to ingest each run's CSV/JSONL evidence
   and emit p50 / p90 / max summaries, response-delay metrics, and
   authority-loss events. See `docs\RUNTIME_LOGGING_AND_EVALUATION.md` for the
   analyzer workflow.
2. Save the decision record produced by `analyze report --out ...` next to the
   manifest under `release\runtime\` or a per-run `runtime\analysis\` folder.
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
