# Source-Aware Blend Decision - 2026-05-26

Status: current implementation decision.

## Problem

The shipped `max_cpu_gpu` blend compares raw CPU Tctl/Tdie and GPU envelope
temperatures before curve lookup. That is safe, but it also lets a CPU-only
rise run through curves that were tuned as GPU-envelope airflow curves. The
separate `cpu_override_curve` then becomes partly misleading on those channels:
the primary curve can already have exceeded the CPU overlay because the CPU
temperature was used as the primary curve input.

The observed effect is useful high-CPU protection, but it is also extra warm
and low-load duty where GPU heat is not the driver.

## Data References

Counterfactual calculations were run against existing runtime CSVs without
starting, stopping, or changing the live controller.

Current shipped-config group:

- Config hash: `036cda22e65c7f06f64f865556cf18771c86caa715b34f00a7acca489c093f06`
- Rows after physically plausible CPU/GPU filtering: `407207`
- Band split: `384104` idle, `20344` warm, `1763` CPU-hot/GPU-cool,
  `995` GPU-hot/CPU-cool, `1` combined-hot
- Recomputed current feed-forward from `config/control.release.json` matched
  the CSV exactly: p95 absolute error `0.000`, max `0.000`

Representative files:

- `release/runtime/logs/archive/svg_mb_control_control-loop_20260526_171414.csv`
  - CPU/Tctl p90/max `68.88/80.88 C`, GPU envelope p90/max `60/64 C`
  - Source-aware below guard saved `4.67` total duty-points/tick on average
    with `0.00` reduction in CPU-hot rows.
- `release/runtime/logs/archive/svg_mb_control_control-loop_20260526_101400.csv`
  - CPU/Tctl p90/max `56.25/76.12 C`, GPU envelope p90/max `46/56 C`
  - Source-aware below guard saved `1.87` total duty-points/tick on average.
- `release/runtime/logs/archive/svg_mb_control_control-loop_20260525_192634.csv`
  - CPU/Tctl p90/max `65.50/87.38 C`, GPU envelope p90/max `66/68 C`
  - Source-aware below guard saved `2.54` total duty-points/tick on average
    while preserving CPU-hot rows.
- Historical CPU-heavy trace:
  `release/runtime/logs/archive/svg_mb_control_control-loop_20260524_024240.csv`
  - CPU/Tctl p90/max `85.62/89.25 C`
  - Unguarded full source-aware would have removed `68.83`
    duty-points/tick in CPU-hot/GPU-cool rows, which is too much CPU support
    to remove without a fresh thermal pass.

## Options Compared

| Option | Current-config average reduction | Warm-row average reduction | CPU-hot reduction | Assessment |
| --- | ---: | ---: | ---: | --- |
| Full source-aware on all `max_cpu_gpu` channels | `2.25` | `5.07` | `72.61` | Too risky: removes high-CPU protection. |
| Source-aware on channels `2` and `3` | `2.02` | `3.32` | `41.74` | Still large CPU-hot reduction. |
| Source-aware on channels `0`, `2`, and `3` | `2.14` | `4.17` | `57.60` | Still large CPU-hot reduction. |
| Source-aware on all `max_cpu_gpu` channels only below `75 C` CPU | `1.93` | `5.07` | `0.00` | Best risk-adjusted return. |
| Source-aware on channels `2` and `3` only below `82 C` CPU | `1.88` | `3.32` | `7.33` | Lower blast radius, lower return. |

The low-band `union` / `sum capped` alternatives did not show meaningful
return in the current data. Current-config rows had zero positive combined
CPU/GPU low-band signal rows, so low-band math is not the best first change.

## Verification Addendum

After implementation, the same read-only counterfactual calculation was rerun
against the expanded live archive for config hash
`036cda22e65c7f06f64f865556cf18771c86caa715b34f00a7acca489c093f06`.
The archive had grown to `19` matching CSV files and `421806` physically
plausible CPU/GPU rows:

- Band split: `390452` idle, `24995` warm, `1528` CPU-hot/GPU-cool,
  `4411` GPU-hot/CPU-cool, and `420` combined-hot rows.
- Recomputed current feed-forward still matched the CSV within serialization
  precision: p95 absolute error `0.00044`, max `0.00050`.
- The representative-file values above still reproduce: for example
  `20260526_171414` gives `68.875/80.875 C` CPU p90/max,
  `60/64 C` GPU p90/max, and `4.6706` duty-points/tick average guarded
  reduction.

Expanded-archive option check:

| Option | Average reduction | Warm-row average reduction | CPU-hot reduction |
| --- | ---: | ---: | ---: |
| Full source-aware on all `max_cpu_gpu` channels | `2.32` | `5.27` | `70.79` |
| Source-aware on channels `2` and `3` | `2.08` | `3.52` | `40.23` |
| Source-aware on channels `0`, `2`, and `3` | `2.20` | `4.35` | `55.73` |
| Source-aware on all `max_cpu_gpu` channels only below `75 C` CPU | `1.99` | `5.27` | `0.00` |
| Source-aware on channels `2` and `3` only below `82 C` CPU | `1.93` | `3.52` | `11.22` |

The expanded archive changes the exact averages but not the decision: the
`75 C` CPU guard preserves CPU-hot behavior while retaining almost all measured
warm-row savings.

Implementation verification on 2026-05-26:

- `build\x64-release\svg-mb-control.exe --show-config --json --config
  config\control.example.json` shows channels `0`, `2`, `3`, and `4` as
  `max_cpu_gpu_source_aware` with `source_aware_cpu_hot_guard_c = 75.0`;
  channels `1` and `5` remain `cpu_only`.
- `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed with CTest `2/2` and
  hermetic unittest discovery `101/101`.
- The live release was checked read-only and was not published or restarted;
  it is still running the old status schema without `last_primary_temp_source`.

## Live Rollout Verification

With operator approval, `.\build-release.ps1 -KeepBuildDir` was run on
2026-05-26. The workflow stopped the packaged controller, temporarily disabled
the watchdog task, built and tested the release, published `release\`, created
`release\archive\svg-mb-control-20260526-1837.zip`, restarted the packaged
controller, and restored the watchdog task state.

Build and release identifiers:

- Commit: `b5c853dfcd36131e20db95ad4c28ccf053d364af`
- Release executable SHA256:
  `985d36c821381c48df204612349281586f3f2fea01d6afe9abb3dc1cec463eb0`
- Published `release\control.json` SHA256:
  `eadef3f7393d361f62735aca91e282d25d6f8cd5d2b90a986c094e90ac63cbc3`
- Tests in release workflow: CTest `2/2`, hermetic unittest discovery `101/101`

Live health after restart:

- `release\svg-mb-control.exe --health --json` reported `healthy`.
- Worker PID `27100` and supervisor PID `6524` were active from `release\`.
- `release\control.json` contained `max_cpu_gpu_source_aware` and
  `source_aware_cpu_hot_guard_c = 75.0` on channels `0`, `2`, `3`, and `4`.
- `control_runtime.json` exposed `last_primary_temp_source`.
- The live CSV header exposed `channelN_primary_temp_source`.

Short live analyzer report:

- Run id: `123`
- Session start: `2026-05-26T18:37:44`
- Sample size: `559` ticks over `140 s`
- Status: `running`
- CPU/Tctl max: `67.375 C`
- GPU envelope max: `48.0 C`
- Robustness: `write_failures=0`, `restore_failures=0`
- Source counts:
  - Channel `0`: `gpu=559`
  - Channel `2`: `gpu=559`
  - Channel `3`: `gpu=558`, `unavailable=1`
  - Channel `4`: `gpu=558`, `unavailable=1`
  - Channel `1`: `cpu=559`
  - Channel `5`: `cpu=558`, `unavailable=1`
  - Channel `6`: `unavailable=559`

This verifies the deployed plumbing and the normal below-guard source-aware path:
the four source-aware channels are using GPU as the primary curve input, while
the CPU-only channels are using CPU. It does not yet prove the CPU guard path or
combined-load thermal behavior because CPU stayed below `75 C` and GPU stayed
cool during the short sample.

## Done And Left

Done:

- Implemented `max_cpu_gpu_source_aware`.
- Implemented `source_aware_cpu_hot_guard_c`.
- Preserved legacy max CPU/GPU behavior at and above the CPU guard.
- Added CPU fallback when GPU telemetry is unavailable but CPU telemetry is
  usable.
- Shipped source-aware blend on channels `0`, `2`, `3`, and `4`; channels `1`
  and `5` remain `cpu_only`.
- Added `last_primary_temp_source` to runtime status and
  `channelN_primary_temp_source` to CSV.
- Added analyzer ingestion and report support for primary source counts.
- Added unit, smoke, config-contract, analyzer, and C++ coverage.
- Published and restarted the packaged release after the full release workflow
  passed.

Left:

- Run a longer normal-use soak and compare noise/duty/temperature against the
  archived old-config baseline.
- Run a CPU-heavy pass that crosses `75 C` to verify `cpu_guard` / `gpu_guard`
  source labels and confirm no CPU-hot duty regression.
- Run a GPU-heavy or combined CPU/GPU pass to verify the intended GPU-primary
  behavior under real GPU load.
- Watch for `cpu_fallback` in source counts; any non-test occurrence means GPU
  telemetry availability should be diagnosed.
- Investigate `analyze ingest --force` against the whole live runtime home:
  the new run ingested and reported successfully, but several older archived
  manifests emitted duplicate `tick_samples` uniqueness errors while force
  reingesting the entire archive.

## Decision

Adopt a guarded source-aware blend mode:

- New blend name: `max_cpu_gpu_source_aware`
- Below `source_aware_cpu_hot_guard_c`, evaluate the primary curve from the GPU
  envelope when GPU telemetry is available, then take the maximum of that
  demand and `cpu_override_curve(cpu)`.
- If GPU telemetry is unavailable below the guard, fall back to CPU as the
  primary curve input rather than entering sensor-safe mode while CPU telemetry
  remains usable.
- At or above `source_aware_cpu_hot_guard_c`, preserve the existing
  `max_cpu_gpu` raw-temperature behavior.
- Ship `source_aware_cpu_hot_guard_c = 75.0` on the current `max_cpu_gpu`
  channels (`0`, `2`, `3`, and `4`).
- Publish `last_primary_temp_source` in status and
  `channelN_primary_temp_source` in CSV so the next run can show whether a row
  used GPU source-aware input, CPU telemetry fallback, or CPU guard fallback.

This targets the measured warm-row savings while retaining the existing
high-CPU response path until a combined-load run proves a lower CPU authority
is safe.
