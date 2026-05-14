# Logging Improvement Plan

## Goal

Make controller tuning data-driven without turning the runtime into a larger
service. Keep the live runtime simple, keep raw logs local by default, and add
just enough structure to compare runs and justify config changes.

## Current Status

- Phase 1 analyzer support is implemented in `scripts\analyze_control_run.py`.
- Runtime manifests are implemented by the controller itself:
  `logs\svg_mb_control_manifest.json` and
  `logs\archive\svg_mb_control_<mode>_<timestamp>.manifest.json`.
- Analysis manifests remain useful as curated decision artifacts that attach a
  profile, notes, and hashes to a chosen run.

## Phase 1: Offline Summaries

Add a repo-owned analyzer for `control-loop` CSV plus
`svg_mb_control_events.jsonl`.

Inputs:

- control-loop CSV path,
- optional event JSONL path,
- optional run label/profile,
- optional workload start and stop timestamps.

Outputs:

- compact Markdown summary for humans,
- compact JSON summary for future tooling.

Minimum metrics:

- row count and wall-clock range,
- CPU/Tctl p50, p90, p99, max,
- GPU core and memory p50, p90, p99, max,
- achieved interval p50, p95, max, and overrun count,
- loop work p50, p95, max,
- process CPU p50, p95, max,
- setpoint p50, p90, max per controlled channel,
- writes per minute per controlled channel,
- max thermal-pressure boost and time at cap per controlled channel,
- downward setpoint steps above the active deadband,
- event counts by event type.

Acceptance:

- Analyzer works against current CSV schema `svg_mb_control.log.v1`.
- Analyzer tolerates comment prologue lines and missing optional fields.
- Summary output is small enough to commit under `docs\` or
  `runtime\analysis\` when it represents a tuning decision.

## Phase 2: Run Manifests

The controller now writes a native runtime manifest during each run. Keep adding
curated analysis manifests beside important summaries when a run supports a
tuning decision.

Minimum fields:

- `run_id`,
- `profile`,
- `started_at`,
- `ended_at`,
- `config_path`,
- `config_sha256`,
- `build_info_path`,
- `git_commit`,
- `runtime_manifest_path`,
- `csv_path`,
- `event_log_path`,
- `operator_notes`,
- `ambient_notes`.

Acceptance:

- A tuning decision can be traced from summary to raw local artifact, config,
  and build.
- Raw CSV captures remain optional to commit; summaries and manifests are the
  durable repo artifact.

## Phase 3: Runtime Identity In Logs

Extend the CSV prologue with identity fields that do not change row shape:

- build version,
- git hash,
- config path,
- config SHA256,
- runtime policy path,
- runtime policy SHA256,
- active control-loop tick and write cooldown.

Acceptance:

- Existing CSV parsers still work.
- A standalone CSV can be traced back to config and binary without consulting
  live status JSON.

## Phase 4: Operational State Fields

Expose important failure state directly in `control_runtime.json`.

Per-channel additions:

- `sensor_failed`,
- `consecutive_sensor_failures`,
- `circuit_breaker_open`,
- `consecutive_write_failures`.

Acceptance:

- Event JSONL remains the durable history.
- Status JSON gives the operator enough current state to see why a channel is no
  longer writing.
- Circuit-breaker reset behavior is explicit. If reset requires process
  restart, document that; if runtime reset is added, log the reset event.

## Phase 5: Deeper Cadence Diagnostics

Only add these if Phase 1 summaries show unexplained jitter or runtime cost:

- AMD read duration,
- GPU read duration,
- SIO fan read duration,
- fan write duration,
- CSV write duration,
- status JSON write duration.

Acceptance:

- Added fields answer a specific timing question.
- They do not obscure the main control-loop CSV or materially increase hot-path
  overhead.

## Ordering

Do Phase 1 first. It gives immediate value from existing logs and avoids changing
the runtime before the current data is easy to compare.

Do Phase 2 and Phase 3 next so future tuning runs are self-describing. Do Phase
4 before depending on `control_runtime.json` for operations. Defer Phase 5 until
there is evidence that per-sensor timing is needed.
