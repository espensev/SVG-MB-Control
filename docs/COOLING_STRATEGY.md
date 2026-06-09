# Cooling Strategy

Status: current as of 2026-05-27.

This is the canonical human-readable reference for the SND-DESK fan
topology, fan inventory, pressure strategy, floor philosophy, and
fan-relationship rules. The machine-readable companion is
`config\machines\snd-desk.cooling.policy.json`, enforced by
`tests\test_machine_cooling_policy.py`. Keep fan types and roles here
mostly in one place; other docs should point here unless they need a
small local summary.

The adopted low-load profile and validation evidence live in
`docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md`. The tuning pass design,
acceptance criteria, and stop conditions live in
`docs\response-evaluation-tuning-plan.md`. The per-tick numerical control
identity lives in `docs\CONTROL_PIPELINE_MATH.md`.

## Scope

- Strategy covers the six airflow fans controlled by this repo's live
  policy: channels `0-5`.
- Channel `6` is AIO/pump scope and is excluded from case pressure math
  and from this strategy.

## Strategy At A Glance

At idle and low load, the goal is to maintain a slight positive case
pressure by running intake airflow above exhaust airflow. This biases
dust ingress toward filtered intake paths and reduces ingress through
unfiltered case gaps.

At higher cooling demand, pressure balance is secondary. Once CPU/GPU
temperatures rise, cooling performance takes priority and the curves
may let intake and exhaust airflow balance out or shift as needed.

Intake configuration:

- `CHA2` / `CHA3`: ASUS ProArt PA602 stock 200 x 38 mm PWM filtered
  front intakes.
- `CHA4`: Noctua NF-A14 industrialPPC-3000 PWM configured as an
  intake on the front radiator.

Channel `4` intentionally belongs to both mental models: it is a
radiator Noctua lane and it is also a filtered intake lane.

The structured form of this strategy is in `snd-desk.cooling.policy.json`
under `strategy.idle_low_load` (`priority = positive_case_pressure`,
`target_intake_to_exhaust_flow_index_ratio_min = 1.1`,
`intake_channels = [2, 3, 4]`, `exhaust_channels = [0, 1, 5]`) and
`strategy.high_load` (`priority = thermal_performance`,
`pressure_balance_may_relax = true` above
`cpu_tctl_c = 72.0` or `gpu_envelope_c = 68.0`).

`tests\test_machine_cooling_policy.py::test_policy_records_positive_pressure_strategy`
fixes the strategy fields. The reference-RPM intake bias is fixed by
`test_reference_static_low_load_profile_is_intake_biased`, which requires
the intake flow-index sum to exceed the exhaust flow-index sum by at least
the configured `target_intake_to_exhaust_flow_index_ratio_min`.

## Fan Inventory

The authoritative machine-readable per-fan data is the `fans[]` array in
`snd-desk.cooling.policy.json`. The table below is the canonical prose
view of that data; if the two diverge, update both and treat the JSON as
the source of truth for tests and packaging.

| Channel | Header | Model | Diameter | Direction | Role | Release min (%) | Reference RPM | Response intent |
|---:|---|---|---:|---|---|---:|---:|---|
| 0 | CHA0 | Noctua NF-A14 industrialPPC-3000 PWM | 140 mm | exhaust | `rear_case_exhaust` | 15.5 | 609 | Lowest exhaust floor at low load (positive-pressure bias); `max_cpu_gpu_source_aware` blend whose GPU `curve` ramps under medium+ load to match the radiator exhausts; CPU assist via `cpu_override_curve` |
| 1 | CHA1 | Noctua NF-A14 industrialPPC PWM (140 mm) | 140 mm | exhaust | `radiator_exhaust` | 22.0 | 457 | CPU response carried by `cpu_override_curve`; `max_cpu_gpu_source_aware` blend with a GPU-airflow assist `curve` (near-floor below ~64 C, ~half-strength by ~75 C, strong by ~82 C, stronger toward ~90 C); not mirrored with channel 5 |
| 2 | CHA2 | ASUS ProArt PA602 stock 200 x 38 mm PWM | 200 mm | intake | `front_case_intake` | 42.0 | 730 | Dynamic low/medium intake leader; strong GPU airflow; moderate CPU assist |
| 3 | CHA3 | ASUS ProArt PA602 stock 200 x 38 mm PWM | 200 mm | intake | `front_case_intake` | 38.0 | 700 | Dynamic low/medium intake leader; resonance-spaced 4% below channel 2 |
| 4 | CHA4 | Noctua NF-A14 industrialPPC-3000 PWM | 140 mm | intake | `front_radiator_intake` | 24.0 | 1081 | Dynamic low/medium intake support; `max_cpu_gpu_source_aware` blend; secondary radiator authority |
| 5 | CHA5 | Noctua NF-A14 industrialPPC PWM (140 mm) | 140 mm | exhaust | `radiator_exhaust` | 20.0 | 735 | CPU response carried by `cpu_override_curve`; `max_cpu_gpu_source_aware` blend with a GPU-airflow assist `curve` (near-floor below ~64 C, ~half-strength by ~75 C, strong by ~82 C, stronger toward ~90 C); not mirrored with channel 1 |
| 6 | CHA6 | AIO/pump scope | n/a | excluded | `aio_pump_scope` | n/a | n/a | Not controlled by this loop; excluded from case pressure math |

Release minimum and reference RPM are the `release_min_duty_pct` and
`reference_static_low_load_rpm` fields from the policy JSON. The reference
RPMs are from the previous static low-load profile and remain the
comparison baseline until the dynamic low-end profile is re-validated live.
The shipped curve parameters that realize each response intent are in
`config\control.release.json` (`control_loop.channels[]`). The configured
pressure-boost terms (`thermal_pressure_*`, `midband_*`, `gpu_airflow_*`,
`cpu_low_soak_*`, `low_band_stage`) and their composition order are defined
in `docs\CONTROL_PIPELINE_MATH.md` §5-6.

The flow-index proxy used by the policy and tests is
`flow_index = rpm / 1000 * (fan_diameter_mm / 120)^2`. It is not a real
CFM measurement; it ignores radiator restriction, fan curves, blade
shape, and case impedance. It is sufficient for keeping the 200 mm and
140 mm channels from being treated as equivalent.

## Floor Philosophy

`min_duty_pct` is the static lower bound applied after curve lookup
(`docs\CONTROL_PIPELINE_MATH.md` §4). It is not the same as the fan's
electrical-mechanical minimum. The policy JSON encodes the intended
semantics:

- `strategy.control_model.hard_min_duty_pct_semantics`:
  `"physical electrical/mechanical minimum plus safety margin only"`.
- `strategy.control_model.avoid_static_high_hard_floors`: `true`.
- `strategy.control_model.use_dynamic_soft_floor_for_low_medium_load`:
  `true`.

The shipped release now uses lower static `min_duty_pct` values on
intake channels `2`/`3`/`4`, then carries the positive-pressure bias with
the first low/medium curve segments instead of high hard floors. The
intake low-end curves are published in the policy JSON as
`response_intent.soft_floor_curve` and copied into both `curve` and
`cpu_override_curve` in `config\control.release.json`. The radiator-lane
floors for exhaust channels `1` and `5` still carry spike-response
headroom; channel `4` is a filtered intake and radiator lane, so its
minimum was lowered but its high-heat radiator response remains.

### Why The Radiator Lanes Should Not Sit At The Absolute Minimum

A CPU boost spike couples heat into the AIO loop on the order of
seconds. Once the spike reaches the radiator, dissipation requires air
flowing across the fins. The control loop applies a rate limit
(`max_setpoint_step_pct` per tick, plus `rise_rate_pct_per_min`,
`docs\CONTROL_PIPELINE_MATH.md` §7-8). For the shipped radiator
channels:

- channel `1`: `rise_rate_pct_per_min = 75`, `max_setpoint_step_pct = 0.8`
- channel `4`: `rise_rate_pct_per_min = 60`, `max_setpoint_step_pct = 0.6`
- channel `5`: `rise_rate_pct_per_min = 75`, `max_setpoint_step_pct = 0.8`

At `75 %/min`, the loop can add at most ~12.5 percentage points of duty
in 10 s, or ~25 percentage points in 20 s. If the radiator fan must
reach `~55 %` duty to dissipate a sustained spike, starting from a
`20 %` floor leaves the radiator at `~32 %` ten seconds into the
ramp; starting from a `28 %` floor leaves it at `~40 %`. The higher
starting point both narrows the ramp distance and gives the radiator
more thermal headroom to absorb the leading edge of the spike before the
loop catches up.

This is the headroom argument: radiator-lane floors should sit above
the noise-acceptable minimum by an amount that gives the rate limiter
useful runway against the worst case the loop is expected to handle.
The intake floors carry pressure bias; the radiator-lane floors carry
spike headroom.

### Floor Heuristics

These are recommendations (not enforced contracts). They are applied
against the tuning passes in
`docs\response-evaluation-tuning-plan.md`.

- Each `min_duty_pct` should sit at the fan's electrical-mechanical
  minimum plus a margin sized for that lane's role:
  - intake lanes (channels `2`, `3`, `4`): margin is whatever idle
    noise budget allows above the pressure-bias requirement
    (`flow_index` sum on `[2, 3, 4]` must exceed the
    `[0, 1, 5]` sum by at least
    `target_intake_to_exhaust_flow_index_ratio_min`);
  - radiator exhaust lanes (channels `1`, `5`): margin sized so the
    rate limiter can close ≥ 50 % of the curve-defined ramp distance
    within 10 s of a sustained Tctl entry into the
    `cpu_override_curve` knee. With the shipped curves and rate
    limits, this puts radiator-exhaust floors in the upper-`20 %` to
    low-`30 %` band before noise constraints. The current shipped
    `22 %` and `20 %` are below this heuristic and are candidates for
    a measured uplift the next tuning pass evaluates.
  - rear case exhaust (channel `0`): stays at the lowest exhaust
    floor; it does not need spike headroom because it does not couple
    to the AIO loop, and a higher floor would harm the pressure
    bias.
- A floor change must be paired with a curve shape that does not
  contradict it. The first curve segment above the floor should join
  the floor smoothly (the shipped curves do this with a flat segment up
  to the first knee, see `config\control.release.json` channel `1`
  `curve`).
- Intake channels `2`, `3`, and `4` use `soft_floor_curve` segments as
  the current low/medium load policy. The first point is the hard
  `min_duty_pct`; later points ramp with temperature so idle and
  mid-band control are not forced by a high static floor.

## Fan-Relationship Rules

The strategy is independent fan responses, not one curve replicated
across channels. The policy JSON encodes this as:

- `strategy.control_model.require_independent_fan_responses`: `true`.
- `groups.front_200mm_intake_pair`:
  `channels = [2, 3]`, `minimum_idle_spacing_pct = 4.0`.
- `groups.radiator_exhaust_pair`:
  `channels = [1, 5]`, `allow_mirrored_response = false`,
  `mirror_group = null` on each fan's `response_intent`.

### Front 200 mm Intake Pair (Channels 2, 3)

- Both fans are the same model and physically symmetric, but identical
  PWM duty would produce same-RPM resonance audible at the desk.
- Idle floor spacing must satisfy `channel 2 floor - channel 3 floor ≥ 4 %`.
  The current release minima (`42 % / 38 %`) meet the rule with no slack.
- The same `≥ 4 %` spacing is required of the `soft_floor_curve` points at
  matched temperatures and is enforced by
  `tests\test_machine_cooling_policy.py::test_soft_floor_curves_are_not_high_static_floors`.
- Both channels are idle/low-load intake leaders. Their `cpu_override_curve`
  and primary `curve` may share shape but must keep the `4 %` offset
  through the low-temperature plateau.

### Radiator Exhaust Pair (Channels 1, 5)

- Both fans drive the AIO radiator. As of 2026-06-09 both use
  `temp_blend = max_cpu_gpu_source_aware` (guard `75 C`): their primary
  `curve` is a GPU-airflow assist that is near-floor below ~64 C (the
  GPU-driven `gpu_airflow` and `midband_pressure` boosts start at 64 C),
  then climbs gradually to about half strength by ~75 C (ch1 ~63 %,
  ch5 ~60 %), strong by ~82 C (~70 %), and stronger toward ~90 C
  (ch1 ~96 %, ch5 ~93 %) — the 86->90 C push carried by the time-gated
  `thermal_pressure` boost. Their setpoint ramps with GPU temperature
  alongside the front intakes; their CPU/radiator response is carried
  through `cpu_override_curve`, which is the elementwise maximum of the
  prior CPU curve and CPU-emergency override. The GPU curve was retuned
  2026-06-09 to reach higher sooner and more gradually; see
  `docs/gpu-response-curve-retune-2026-06-09.md` and
  `docs/radiator-exhaust-gpu-response-decision-2026-06-09.md`.
- Their curves must not be a single shape applied twice. On the GPU assist
  `curve` the two lanes share temperature knots but channel `1` stays strictly
  above channel `5` by `>= 2 %` at every knot (no crossing), enforced by
  `test_radiator_exhaust_gpu_curves_are_staggered_not_mirrored` — the analogue
  of the intake pair's `>= 4 %` spacing guard. On the CPU path the
  `cpu_override_curve`s remain near-identical; CPU-side separation rests on the
  floor (`22 %` vs `20 %`) and `decay_latch_above_pct` (`38 %` vs `32 %`).
- `test_radiator_exhaust_pair_is_not_mirrored_and_stays_above_rear_exhaust`
  fixes the no-mirror flag and requires both radiator-exhaust floors to
  exceed the rear-exhaust floor.

### Cross-Lane Independence

- Channel `4` (front radiator Noctua intake) uses
  `temp_blend = max_cpu_gpu_source_aware` with
  `thermal_pressure_max_boost_pct = 14.0`. Channels `1` and `5` use
  `max_cpu_gpu_source_aware` (since 2026-06-09) with
  `thermal_pressure_max_boost_pct = 20.0`; their CPU authority is carried
  by `cpu_override_curve`. Channels `1` and `5` therefore hold a higher
  high-load CPU duty ceiling than channel `4` (the `20.0` versus `14.0`
  boost plus steeper `cpu_override_curve` knees), and the three radiator
  lanes must not be re-merged into a single curve shape.
- Channel `0` carries a different curve role (smallest exhaust assist).
  Its `cpu_override_curve` knee is below those on channels `1` and `5`
  by design.

### When To Allow Two Channels To Run Close

The independence rule is not absolute. Two channels may run at
near-identical setpoints when:

- both are above the audible-resonance threshold (typical for radiator
  channels at high load),
- both have been driven there by the same temperature input through
  curves that are independently shaped (i.e., the convergence is a
  result, not a configured equality), and
- the convergence is in the high-load region where cooling performance
  has priority over pressure and noise.

In the low-load region, channels in the same group must remain
separated by enough to avoid the resonance band.

## How To Apply During Tuning

Before adjusting a curve point, floor, or boost term, check:

1. **Pressure bias preserved?** If the change is in the low-load band
   (Tctl < `72 C` and GPU envelope < `68 C`), would the post-change
   `flow_index` on intakes `[2, 3, 4]` still exceed the
   `[0, 1, 5]` sum by `target_intake_to_exhaust_flow_index_ratio_min`?
   If not, raise an intake before lowering an exhaust.
2. **Front-pair spacing preserved?** If touching channel `2` or `3`,
   does the new floor or curve point at idle keep the `≥ 4 %`
   spacing? Re-run
   `tests\test_machine_cooling_policy.py::test_soft_floor_curves_are_not_high_static_floors`
   after the change.
3. **Radiator pair not mirrored?** If touching channel `1` or `5`,
   does at least one curve breakpoint, `decay_latch_above_pct`,
   `thermal_pressure_*`, or `cpu_low_soak_*` field differ between the
   two channels after the change?
4. **Channel role respected?** Does the change agree with the
   channel's `response_intent` (e.g., do not give channel `4` the
   high-load CPU authority carried by channels `1` and `5`; do not
   lower channel `2` or `3` below their idle-leader floor unless the
   intake bias still clears the ratio).
5. **Floor change has spike-response justification?** If the change
   raises a radiator-lane floor, name the spike scenario it covers
   (which Tctl knee, which rate limit, which dwell) and the noise
   budget it consumes. If the change lowers a radiator-lane floor,
   name the noise observation that justifies it and the spike-response
   measurement that confirms headroom is still adequate.
6. **Evidence trail?** Record the change in the tuning pass record
   (`docs\response-evaluation-tuning-plan.md`) with the validation
   evidence required by `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md`
   (steady-state `last_setpoint_pct`, `low_band_evidence.json` counts,
   `loop_slip_ms`, breaker state) before treating the change as
   adopted.

## References

- `config\machines\snd-desk.cooling.policy.json` — machine-readable
  policy (strategy, groups, fans, response intent, soft-floor curves).
- `tests\test_machine_cooling_policy.py` — enforced strategy
  contracts.
- `config\control.release.json` — shipped per-channel curves,
  smoothing, boost terms, and rate limits.
- `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` — adopted low-load profile
  and validation evidence.
- `docs\response-evaluation-tuning-plan.md` — pass design and
  acceptance criteria.
- `docs\CONTROL_PIPELINE_MATH.md` — per-tick numerical control
  identity (curve, smoothing, boosts, rate limit, low-band).
- ASUS ProArt PA602 product page: <https://www.asus.com/us/motherboards-components/cases/proart/proart-pa602/>
- ASUS PA602 press release, including front 200 mm fan airflow note: <https://press.asus.com/news/press-releases/asus-proart-pa602-chassis/>
- Noctua NF-A14 industrialPPC-3000 PWM specifications: <https://www.noctua.at/en/products/nf-a14-industrialppc-3000-pwm/specification>
- Noctua NF-A14 industrialPPC-2000 PWM specifications: <https://www.noctua.at/en/products/nf-a14-industrialppc-2000-pwm/specifications>
- `D:\Development\Thermals\SVG-MB\docs\hardware-documentation\` — fan
  datasheets and case documentation outside this repo.
