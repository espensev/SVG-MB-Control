# Fan, Power, Load, and Response Review - 2026-06-26

Scope: read-only review of local `release\runtime` telemetry for fan behavior,
historical temperature drift, power logging, CPU/GPU load context, and controller
response. No live runtime control action was taken.

## Evidence

- Runtime sidecars:
  - `release\runtime\control_runtime.json`
  - `release\runtime\current_state.json`
  - `release\runtime\logs\svg_mb_control_manifest.json`
  - `release\runtime\logs\svg_mb_control_events.jsonl`
- Runtime CSV archives:
  - `release\runtime\logs\archive\svg_mb_control_control-loop_*.csv`
  - `112` control-loop CSV archives from `2026-06-19` through `2026-06-26`
  - `1,846,971` streamed rows in the response pass
  - The active mutable CSV during this review was
    `svg_mb_control_control-loop_20260626_062247.csv`; historical conclusions
    should be read from closed archives, while current-state claims come from
    sidecars.
- Parsing method:
  - CSV rows were streamed by header name using the same comment/header split
    contract as `scripts\control_csv.py`.
  - CCD temperatures were parsed with `scripts\control_csv.py`
    `parse_ccd_temps`.
  - CPU package power was derived only from distinct nonzero
    `cpu_power_sample_id` windows:
    `(cpu_pkg_energy_delta_uj / 1e6) / (cpu_power_window_ms / 1000)`.
  - GPU power was summarized over distinct nonzero `gpu_power_sample_id`
    readings as instantaneous board watts.

## Current Point-In-Time State

Sidecars read at `2026-06-26T06:57:35` reported:

- Runtime status: `running`, `control-loop`, tick `8280`.
- Timing: `loop_achieved_interval_ms=250.4491`,
  `loop_work_duration_ms=44.0914`, `process_cpu_pct=0.0779`.
- Temperatures: CPU Tctl/Tdie `56.125 C`, CCD1 `41.375 C`, CCD2 `52.5 C`,
  GPU core `32.426 C`, GPU memory junction `44.0 C`.
- Fan B / channel 5 / `radiator_exhaust_b`: duty `20.0%`, RPM `734`,
  setpoint `20.035%`, observed temp `44.0 C`, response source
  `primary_curve`, primary source `gpu`.

This is a normal near-floor point. It is not evidence of fan-B runaway or stuck
high duty.

## Fan B / Channel 5 Behavior

`fan B` maps to channel `5` / `CHA5` / `radiator_exhaust_b`, with a release
floor of `20%`.

Same-temperature band used for the current comparison:
`cpu_tctl_c 55-60 C` and `gpu_memjn_c 39-45 C`.

Historical same-temperature distribution:

| Metric | n | p50 | p90 | p95 | max |
|---|---:|---:|---:|---:|---:|
| fan5 duty % | 260,459 | 20.0 | 20.39 | 21.18 | 62.75 |
| fan5 RPM | 260,459 | 736 | 750 | 776 | 1906 |
| channel5 setpoint % | 260,458 | 20.001 | 20.202 | 20.618 | 62.4 |

Interpretation:

- The current fan-B point, `20.0% / 734 RPM`, is essentially at the historical
  same-temperature median.
- Median same-temperature fan-B behavior is stable by day: duty stays at
  `20.0%`, RPM stays about `734-736`, and setpoint stays near `20.0%`.
- The upper tail widened on `2026-06-25` (`same-temp p95 duty 25.49%`,
  p95 RPM `919`) because the same temperature band can include load-transient
  rows where boost or prior heat history had not decayed yet. That is a
  transient/context effect, not a median drift of fan B.

## Overall Fan Response

Controlled fan duty tracks commanded setpoint closely.

For channel 5:

- Absolute setpoint-duty error: p50 `0.016 pct`, p90 `0.373 pct`,
  p95 `0.390 pct`.
- Write reasons: mostly `none`; `setpoint_delta` rows were present when demand
  changed; `authority_reassert` was rare.
- No current fan-write failure evidence appeared in the active event stream.

Channel 5 median setpoint by observed-temp bin:

| Observed temp bin | n | p50 setpoint | p90 setpoint | p95 setpoint |
|---|---:|---:|---:|---:|
| `<50 C` | 1,527,114 | 20.002 | 20.349 | 20.956 |
| `50-60 C` | 91,415 | 20.025 | 22.200 | 25.935 |
| `60-64 C` | 11,638 | 21.480 | 38.658 | 44.396 |
| `64-68 C` | 138,215 | 20.877 | 27.223 | 32.356 |
| `68-75 C` | 57,053 | 41.624 | 51.336 | 53.818 |
| `75-82 C` | 20,326 | 56.044 | 59.247 | 60.337 |
| `82+ C` | 1,164 | 66.308 | 67.345 | 67.433 |

This matches the documented shape: channel 5 is near floor below the GPU-assist
region, then becomes meaningfully active once observed temperature is in the
upper bands.

The intake channels lead the exhaust lanes under load as intended:

- In the `68-75 C` observed-temp bin, median setpoints were approximately:
  channel 2 `70.754%`, channel 3 `66.763%`, channel 4 `47.864%`,
  channel 1 `44.293%`, channel 5 `41.624%`.
- Channel 1 and channel 5 are not mirrored: channel 1 stays above channel 5 in
  high-response bins, matching the staggered radiator-exhaust policy.

Channel 5 hot-onset scan:

- `800` meaningful hot onsets were found.
- Time to `>=25%` setpoint: n `391`, p50 `0 s`, p90 `8 s`,
  p95 `17.5 s`, max `287 s`.
- Time to `>=30%` setpoint: n `225`, p50 `0 s`, p90 `10 s`,
  p95 `15.8 s`, max `348 s`.

This is a coarse onset scan, not a controlled step test. A `0 s` result often
means the row was already at the setpoint threshold when the bin transition was
observed. The high outliers need event-by-event inspection before treating them
as fan lag.

## Pressure / Airflow Drift

Using the documented proxy:
`flow_index = rpm / 1000 * (fan_diameter_mm / 120)^2`.

Low-load pressure result:

| Band | n | p50 ratio | p90 | p95 | min | rows below 1.1 |
|---|---:|---:|---:|---:|---:|---:|
| Doc low-load: CPU `<72 C`, GPU mem `<68 C` | 1,782,278 | 2.026 | 2.108 | 2.122 | 0.981 | 1,286 |
| Tight low-load: CPU `<65 C`, GPU mem `<55 C`, system CPU `<25%` | 1,555,287 | 2.018 | 2.100 | 2.114 | 0.984 | 924 |

Interpretation:

- The low-load intake/exhaust proxy is comfortably above the `1.1` target in
  the median case.
- Below-target rows are about `0.06-0.07%` of low-load samples, so the positive
  pressure strategy is not drifting in normal operation.
- The below-target minimum rows are likely transient or edge rows; they are not
  representative of the low-load state.

## Temperature History

Pooled archive summary:

| Metric | n | p50 | p90 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| CPU Tctl/Tdie C | 1,845,868 | 57.5 | 64.125 | 65.875 | 75.5 | 91.0 |
| CPU max C | 1,845,868 | 57.875 | 64.5 | 66.5 | 75.5 | 100.875 |
| GPU core C | 1,845,962 | 33.941 | 37.328 | 39.078 | 57.109 | 64.227 |
| GPU mem junction C | 1,845,962 | 46.0 | 48.0 | 50.0 | 70.0 | 74.0 |
| CCD2 - CCD1 C | 1,845,864 | 9.5 | 17.625 | 19.5 | 22.5 | 31.5 |

Day-level drift notes:

- `2026-06-21` was generally warmer than adjacent days
  (CPU p50 `61.75 C`, p90 `66.125 C`).
- `2026-06-24` was cooler
  (CPU p50 `52.375 C`, GPU mem p50 `42.0 C`).
- `2026-06-25` had the strongest CPU tail
  (CPU p95 `75.25 C`, max `91.0 C`) and coincides with known host-load /
  shutdown-adjacent evidence.
- `2026-06-26` had a GPU-memory tail
  (GPU mem p95 `70.0 C`, max `72.0 C`) while median CPU was lower
  (`55.875 C`).

## Power Logging And Load Context

GPU board power:

- Coverage: `gpu_power_acquisition=nvml` across `1,845,950` rows.
- Files with NVML GPU power: `108 / 112`.
- Distinct GPU power samples summarized as board watts:
  p50 `96.696 W`, p90 `113.111 W`, p95 `131.830 W`,
  p99 `492.633 W`, max `603.764 W`.
- GPU workload context: `gpu_context_acquisition=nvml` on `1,648,699` rows.
  GPU utilization p50 `4%`, p90 `12%`, p95 `25%`, p99 `97%`.

CPU package power:

- Coverage: package-energy columns are present and populated in `108 / 112`
  files.
- Provenance is `cpu_pkg_energy_acquisition=quarantine` across the populated
  rows. Per docs, `quarantine` means collected but not promoted to trusted
  `validated`; use it as provisional thermal context, not final efficiency
  proof.
- Derived package watts over distinct nonzero sample IDs:
  p50 `63.211 W`, p90 `96.842 W`, p95 `112.098 W`,
  p99 `172.235 W`, max `211.136 W`.

Approximate CPU package plus GPU board total:

- p50 `161.757 W`, p90 `211.505 W`, p95 `260.826 W`,
  p99 `576.795 W`, max `766.607 W`.
- This is not wall power and not a full-system energy integral. It combines
  quarantine CPU package power with instantaneous GPU board power and excludes
  the rest of the platform.

CPU load telemetry:

- `system_cpu_busy_pct`: p50 `12.684%`, p90 `26.476%`, p95 `34.182%`,
  p99 `99.883%`, max `100%`.
- Controller process CPU stayed tiny: p50 `0.0%`, p95 `0.117%`,
  max `0.487%`.
- CPU cycles were mostly unavailable: `cpu_cycles_acquisition=disabled` for
  most rows, `quarantine` on `6,504` rows. Per-core APERF/MPERF samples existed
  in `6` files; all-core samples existed in `2` files.
- Available all-core cycle ratio evidence looked internally coherent:
  all-core APERF/MPERF ratio p50 `1.2285`, p90 `1.2611`, and contributing cores
  consistently `32`.

Load determination:

- For fan/thermal response, the best current load context is:
  `system_cpu_busy_pct`, provisional CPU package watts, GPU board watts,
  GPU utilization/context, CPU/GPU temperatures, and response-source fields.
- For CPU efficiency, the data is not enough yet for a final verdict because
  package energy is still `quarantine` and cycle coverage is sparse. We can
  characterize load and thermal response, but not confidently score
  cycles-per-Joule across settings from this archive set alone.

## Event-Log Context

All active plus archived event streams:

- `790,569` parsed events, `377` malformed/truncated lines.
- Severity counts: `788,759 info`, `1,665 error`, `145 warning`.
- Top non-info historical classes were sidecar upsert failures, low-band
  evidence write failures, sensor failure/recovery pairs, and supervisor worker
  exits/restarts.

Active event file only:

- `141` events: `139 info`, `1 error`, `1 warning`.
- The non-info pair was at `2026-06-26T02:50:09`:
  `supervisor.worker_exited` followed by `supervisor.worker_restart_scheduled`.
- No active event evidence shows current fan write failure, circuit breaker
  opening, or persistent sensor failure.

## Findings

1. Fan B is behaving normally at the current thermal point. Its present
   `20.0% / 734 RPM` is the expected floor behavior for `56 C` CPU /
   `44 C` GPU-memory conditions.
2. Historical fan-B median drift is not visible. At the same temperature band,
   the median stays at `20.0%` and roughly `734-736 RPM`; the high tail comes
   from transient load/response context, especially on `2026-06-25`.
3. Controlled fan duty follows setpoint. Channel 5 p95 setpoint-duty error is
   about `0.39 pct`, so the actuator path is not the weak link in this data.
4. Intake bias is strong at low load. The flow proxy median is about `2.0x`
   intake/exhaust against a `1.1x` target, with below-target rows under `0.1%`.
5. GPU power logging is strong and useful. NVML board power is present across
   nearly all current archives and can explain the GPU memory-temperature tail.
6. CPU package power is useful but still provisional. It is populated, but its
   acquisition state is `quarantine`, not `validated`.
7. CPU load can be characterized, but CPU efficiency cannot be finalized from
   this archive set. `system_cpu_busy_pct` and package watts give load context;
   APERF/MPERF cycle data is too sparse for robust work-per-energy evaluation.
8. The previous shutdown-adjacent behavior remains a scheduling/load problem
   more than a fan authority problem: process CPU is tiny, but host CPU reaches
   near saturation in the tail windows, and fan response starts once the loop is
   scheduled.

## Recommended Next Read-Only Checks

1. For fan tuning decisions, compare closed archives by same CPU package-watt
   band, same GPU power band, and same response-source class. Do not compare by
   raw Tctl alone.
2. Promote or re-run the CPU package-energy validation path before treating
   package watts as trusted tuning evidence.
3. Enable or collect a longer CPU cycles/all-core capture when the goal is
   "same work for less power"; the current archive set mostly lacks that work
   numerator.
4. For fan-B floor changes, use the documented radiator-floor heuristic plus a
   controlled spike segment. The current evidence does not justify lowering fan
   B below its `20%` floor.
