# Runtime Logging and Data Evaluation

## Status

Current as of 2026-06-20.

The packaged controller is now good enough for measured tuning of the current
profile: channels `0,1,2,3,4,5`, channel `6` blocked by live policy,
`control_loop.poll_tick_ms=250`, `write_cooldown_ms=250`, and
`deadband_pct<=0.25` in the shipped configs.

The completed implementation sequencing is summarized in
`docs\archive\implemented-plans\LOGGING_IMPROVEMENT_PLAN.md`; the current
operator workflow lives here. Later FEAT-0020/0021/0022 logging and analyzer
additions are reflected below and in the owning feature specs.

**Finding (closed 2026-06-09) - active package header drift (2026-06-07):** on
2026-06-07 the live packaged runtime was a stale `2026-05-28T12:15:11Z` build
(source commit `15606140c239`) whose active control-loop CSV session
`2026-06-07T01:10:38` had `247` columns and no `system_cpu_*` columns, even
though current source and tests define the FEAT-0002 whole-system CPU fields.
The 2026-06-09 rebuild/publish closed this: the live session
`2026-06-09T02:32:40` (git_hash `dd2c02214128`, `256` columns) contains all five
`system_cpu_*` columns plus the off-by-default FEAT-0006 `cpu_pkg_energy_*`
columns. Only genuinely older archives that predate the rebuild lack the
`system_cpu_*` columns; bind columns by header name and treat only those as
pre-FEAT-0002 for whole-system CPU analysis.

Earlier local evidence from the previous `50 ms` profile:

- A local-only `20260514_033423` control-loop CSV (not committed) contained
  19,432 rows. CPU/Tctl max was `86.625 C`, achieved loop interval averaged
  `50.641 ms`, radiator channel setpoints reached `71.40%`, `68.00%`, and
  `65.86%`, and the live status showed no overrun with process CPU around
  `0.29%`.
- A local-only `20260514_035931` control-loop CSV (not committed) contained
  17,773 rows from a lower-heat/idle recovery run. CPU/Tctl max was
  `77.500 C`, achieved loop interval averaged `50.610 ms`, max interval was
  `58.150 ms`, average loop work was `4.314 ms`, average process CPU was
  `0.207%`, and no overrun rows were recorded.

Raw captures are local evidence. Do not commit full runtime CSV captures by
default; commit small summaries and decision records instead.

The numerical control-pipeline contract lives in
`docs\CONTROL_PIPELINE_MATH.md`. Keep that document updated when runtime data
shows a changed control identity, when status/CSV fields move, or when source
changes alter curve lookup, smoothing, boost composition, low-band behavior, or
adaptive cadence.

## Current Logging Surfaces

Control owns the runtime logging plane:

- `current_state.json` is the current telemetry snapshot.
- `control_runtime.json` is the current mode/status publication. For
  `control-loop`, schema version `4` includes the active worker PID, timing
  fields, process CPU and memory fields, active log paths, and per-channel
  demand/setpoint state.
- `pending_writes.json` is the restore/recovery sidecar for active writes.
- `stop.request.json` is the cooperative lifecycle request written by
  `svg-mb-control --stop` and consumed by `read-loop` and `control-loop`.
- `logs\svg_mb_control_output.csv` is the fixed-path live CSV mirror of the
  active chunk.
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv` is the retained CSV
  history.
- `logs\svg_mb_control_events.jsonl` is the active event stream for starts,
  rotations, writes, restores, policy refusals, sensor failures,
  circuit-breaker transitions, and sidecar warnings. It rotates into
  `logs\archive\*_events_<timestamp>.jsonl` on the configured active-file
  retention window. The deadline is independent of last-write mtime, so frequent
  appends do not postpone rotation; routine `control_loop.write_applied` info
  events are sampled while diagnostic and lifecycle events remain persisted.
- `logs\svg_mb_control_manifest.json` is the latest native runtime manifest.
  It points at the active archive CSV, live CSV mirror, event log, archive
  manifest, and records row/event counts plus producer identity. Its
  `external_logging.required=false` field is intentional: normal controller
  logging should use this plane, not HWiNFO.
- `logs\archive\svg_mb_control_<mode>_<timestamp>.manifest.json` is the
  per-archive manifest for the matching CSV chunk.
- `logging_health.json` is the last-resort event-log health sidecar at the
  runtime-home root. It records active/recovered event-log append failure without
  relying on the failed event stream.

`control_runtime.json` is intentionally a status view, not the per-tick data
source. Use the CSV for timing and response analysis.

## What Is Working

- The logging plane is product-owned inside `SVG-MB-Control`; it does not need a
  sibling process or bridge helper.
- Control logs per-tick telemetry, fan state, setpoints, feedforward/correction
  split, primary CPU/GPU/guard source attribution, thermal-pressure boost,
  timing quality, and process resource cost.
- The logging plane is observable enough for the shipped channel set under
  local testing. Treat the old `50 ms` captures above as historical unless a
  fresh run is collected with the current shipped config.
- JSONL events separate discrete control actions and failures from the dense CSV
  stream.
- Rotation and retention are local config fields for CSV and event JSONL, so
  long runs do not require external cleanup tooling for active runtime logs.
- Runtime manifests now make a Control run self-describing enough for normal
  validation without an external logger.

This is a solid early-phase data substrate. The next improvement should be
better experiment accounting and automated summarization, not a wholesale
logging replacement.

## Tooling Now Available

- Native `svg-mb-control analyze ingest` imports runtime manifests, CSV archives,
  events JSONL, and plant-model captures into the SQLite analysis DB.
- Native `svg-mb-control analyze report` summarizes one ingested run and can
  write a report, compact Markdown decision record, and analysis manifest with
  artifact hashes. Use this as the default evidence path for new runs.
- Native `svg-mb-control analyze ingest --csv <path> [--events <path>]` ingests a
  bare control-loop CSV with no runtime manifest, and `analyze report` adds a
  GPU-envelope-peak block (`--gpu-load-threshold-c`), a loop-timing and
  process-resource percentile section, and a per-channel low-band-inclusive
  response-boost total.
- `scripts\analyze_control_run.py` is a thin convenience wrapper for captures
  that have not been ingested: it ingests the CSV into a temporary DB with the
  in-repo `svg-mb-control.exe` and forwards native `analyze report` output. All
  analysis is native; the script reimplements nothing.
- `scripts\extract_cpu_aio_segments.py` extracts compact CPU/AIO response
  windows directly from control-loop CSV logs. It treats channels `1`, `4`, and
  `5` as the AIO radiator fan group regardless of intake/exhaust direction,
  keeps channels `0`, `2`, and `3` as context airflow, and defaults summary
  metrics to stop at the first GPU-power/GPU-memory confound while still writing
  the full segment trace for review.
- `scripts\analyze_cpu_temp_power.py` summarizes CPU/Tctl by sustained CPU
  package-power bands from the standard control-loop CSV. It de-duplicates the
  mirrored FEAT-0006 package-energy rows by `cpu_power_sample_id`, derives
  package watts, applies a same-power-band dwell gate, and carries GPU power /
  GPU memory plus policy-marked radiator response context so temperature
  comparisons are not ranked by raw Tctl alone. It also surfaces per-CCD Tdie
  (`ccd1_tdie_c`/`ccd2_tdie_c` and the `CCD2-CCD1 C` balance) parsed from the
  existing `amd_sensor_summary` text column via `control_csv.parse_ccd_temps`, so
  the frequency die (CCD2) versus V-cache die (CCD1) split is visible without a
  schema change.
- `scripts\cpu_config_fingerprint.py` (skeleton) builds a per-run config-pure
  fingerprint (idle package-power floor, per-busy-band watts, effective MHz/W,
  CCD balance) and segments runs into auto-labeled regimes via median/MAD step
  detection layered on exact `git_hash`/`config_sha256` cuts, to detect
  BIOS/Curve-Optimizer/PBO changes from telemetry without operator annotation.
  Cooling-output scalars (`theta`, Tctl-at-watt) are reported but never drive
  segmentation. Effective MHz/W needs a `CPU_CYCLES_MODE=enabled` capture and
  Vcore needs the SVI probe (docs/cpu-cycles-capture-and-vcore-probe-plan-2026-06-21.md);
  those dimensions degrade to null until then. Output schema
  `svg_mb_control.cpu_fingerprint.v1`.
- Runtime CSV comment prologues include producer version, git hash, config
  path/SHA256, runtime-policy path/SHA256, and control-loop tick/write cooldown
  when applicable. A standalone CSV is therefore traceable without the live
  status JSON.
- Foreground `evidence-log` CSV rows include per-backend read durations, poll
  interval, and change flags for runtime snapshot, AMD, GPU thermal, fan state,
  SIO evidence, and GPU evidence fields.
- `Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For <duration> -EvidenceLog`
  uses that foreground `evidence-log` plane during a bounded intentional
  control-off window. This is the supported way to keep read-only telemetry
  logging active while Control is stopped; it writes separate
  `svg_mb_control_evidence.*` CSV/events/manifest files and is stopped before
  Control resumes.
- JSONL runtime events include normalized `severity` and `error_code` fields.
  Non-fault events use `severity=info` and `error_code=none`; warning, error,
  and critical rows use stable uppercase codes derived from `event_type` unless
  the caller supplies an explicit code.
- FEAT-0022 Slice A adds logging-health events for CSV sink visibility:
  `runtime_logging.csv_write_failed` and
  `runtime_logging.csv_write_recovered`. The event `mode` identifies
  `control-loop`, `read-loop`, or `evidence-log`; failure rows include
  `log_csv_path`, `event_log_path`, and a detail string with the failing
  logger sink when available. These events are rate-limited by an in-memory
  failure-active flag so a persistent CSV sink failure emits one failure event
  and one recovery event after a successful write.
- FEAT-0022 Slice B adds `logging_health.json` for event-log-unwritable
  visibility. A persistent event-log append failure writes one active marker
  instead of one fallback file rewrite per event, and the marker is rewritten as
  recovered after the next successful append. `--health --json` and
  `--status --json` include `logging_health_*` / `event_log_failure_*` fields
  and degrade an active otherwise-healthy runtime while the marker is active.
- FEAT-0022 also adds sticky status/snapshot publication visibility:
  `runtime_logging.status_publish_failed`,
  `runtime_logging.status_publish_recovered`,
  `runtime_logging.snapshot_publish_failed`, and
  `runtime_logging.snapshot_publish_recovered`. Failed control-loop status
  publication keeps the forced retry active for the next tick, and failed
  control-loop `current_state.json` publication advances retry timing only after
  a successful write.
- FEAT-0022 Slice C adds analyzer diagnostics for CSV manifest/archive/latest
  mirror consistency. `analyze report` reads the runtime manifest's
  `artifacts.csv_latest.path` when available, reports `csv_latest_row_count`,
  and emits `running_csv_manifest_consistency_warning` for running-session row
  count disagreement or `closed_csv_manifest_consistency_suspect_evidence` for
  closed-run disagreement.

## Remaining Gaps

- CSV chunk files have no closed/ready marker. A reader must treat the active
  archive path as mutable while Control is running. Analyzer reports now flag
  count disagreement as a running warning versus a closed-run suspect-evidence
  diagnostic, but strict comparisons should still use pinned closed archives.
- The current-source control-loop CSV has loop timing, process cost, and, after
  FEAT-0002 is present in the packaged binary, whole-system CPU busy time
  (`system_cpu_busy_pct` plus the raw idle/kernel/user deltas and processor
  count). Older archives and stale packages can lack these columns; consumers
  must bind by header and treat absent fields as unavailable, never as zero.
  The control-loop CSV still does not have per-sensor-group read durations. Use
  foreground `evidence-log` for deeper backend timing/cadence diagnosis unless
  control-loop evidence proves this must move into the hot path.
- Whole-system CPU busy time measures *time*, not CPU *work*. The FEAT-0006 layer
  adds **read-only AMD RAPL package energy** (off by default; enable with
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`): `cpu_pkg_energy_delta_uj` over
  `cpu_power_window_ms`, keyed by `cpu_power_sample_id`, with provenance in
  `cpu_pkg_energy_acquisition` (the live loop emits `disabled`/`unavailable`/
  `quarantine`; `validated` is applied post-capture by the promotion script, never
  by the running worker).
  Average package power is derived, not logged:
  `(cpu_pkg_energy_delta_uj / 1e6) / (cpu_power_window_ms / 1000)` over distinct
  sample ids — heat dissipated (average package watts); pairing it with fan RPM
  to get cooling watts-per-RPM is downstream analysis, not a logged or derived
  field. The FEAT-0006 cycle layer adds **read-only AMD APERF/MPERF** (off by
  default; enable with `SVG_MB_CONTROL_CPU_CYCLES_MODE=enabled`): raw
  `cpu_aperf_delta` (delivered cycles, the work numerator) and `cpu_mperf_delta`
  over `cpu_cycles_window_ms`, keyed by `cpu_cycles_sample_id`, with provenance in
  `cpu_cycles_acquisition`. Read per-core under a transient affinity pin from the
  AMD read-only aliases `0xC00000E7`/`0xC00000E8` (the 2026-06-07 `#GP` was a
  probe-index error). Effective frequency is derived by `analyze report`
  (schema v10), not logged: the cycle-weighted `cpu_aperf_delta /
  cpu_mperf_delta` ratio over distinct `cpu_cycles_sample_id` windows always,
  multiplied to MHz only when `--p0-mhz <mhz>` supplies the base frequency (no
  logged field records P0). Cycles-per-Joule remains a later derivation — the
  energy and cycle windows carry separate sample ids and no join rule is
  specified yet (see `docs/cpu-cycle-counter-source-decision-2026-06-07.md`).
  `system_cpu_busy_pct` remains the time-normalization context, not a substitute.
- The FEAT-0006 **all-core** layer (same `SVG_MB_CONTROL_CPU_CYCLES_MODE` gate,
  off by default) adds the package effective frequency: `cpu_aperf_delta_allcore`
  / `cpu_mperf_delta_allcore` (Σ-dAPERF / Σ-dMPERF over all logical processors)
  over `cpu_cycles_window_ms_allcore`, keyed by `cpu_cycles_allcore_sample_id`,
  with `cpu_cycles_allcore_cores` recording how many cores contributed a fresh
  window. These are produced by a dedicated OFF-THREAD sweeper (its own PawnIO
  handle and affinity; the 32-way affinity sweep never runs on the 250 ms control
  thread), so the all-core window has its OWN sample-id cadence — it must NOT be
  joined to the per-core `cpu_cycles_sample_id`. `analyze report` derives the
  package ratio / effective frequency (a `cpu_cycles_allcore` block with the
  contributing-core max/min) from a separate `GROUP BY cpu_cycles_allcore_sample_id`
  query (analyze schema v13; pre-v13 archives degrade that block to
  `unavailable`). The per-core block is retained unchanged. Effective frequency
  is analyzer evidence only — not a control input.
- The FEAT-0020 layer adds **read-only GPU board power** to the same standard
  control-loop CSV: `gpu_power_mw` (instantaneous NVML board milliwatts), keyed by
  `gpu_power_sample_id` and stamped with `gpu_power_time_ms` (the GPU power read
  timestamp), with the source in `gpu_power_source` (`nvml`/`unknown`) and
  provenance in `gpu_power_acquisition` (`disabled`/`unavailable`/`nvml`). It is
  one `nvmlDeviceGetPowerUsage` read added to the per-tick GPU thermal sample (not
  the foreground `evidence-log` path) and is logging-only — never a control input.
  Because the value is instantaneous (not an accumulating energy counter),
  `analyze report` summarizes it as mean / p50 / p90 / max over distinct
  `gpu_power_sample_id` samples (introduced in analyzer schema v11), **not** the
  time-weighted Sigma-energy integral used for CPU package power. GPU power needs
  no env gate; it records whenever NVML returns a nonzero reading. To turn on the comparable CPU
  package-energy columns in the standard loop (and have the profile survive the
  boot/logon safety revert), use `scripts/Set-EnergyLoggingProfile.ps1 -Enable` /
  `-Disable` (`-DryRun` previews without touching live runtime); the live flip
  needs explicit live-runtime authorization.
- The FEAT-0021 layer adds **read-only GPU workload context** beside GPU power:
  `gpu_context_sample_id`, `gpu_context_time_ms`,
  `gpu_context_sample_age_ms`, `gpu_context_acquisition`,
  `gpu_util_gpu_pct`, `gpu_util_mem_pct`, `gpu_pstate`,
  `gpu_clock_graphics_mhz`, `gpu_clock_memory_mhz`, `gpu_vram_used_mb`, and
  `gpu_vram_total_mb`. It is a cached in-repo GPU reader sample refreshed at
  most once per 1000 ms, not another per-tick wide read. Rows between refreshes
  repeat the same context sample id and carry an increasing sample age. Analyzer
  schema v12 summarizes context only when present and treats older archives as
  unavailable. Context is logging-only and never a response source, write gate,
  breaker input, or fan-duty input.
- The FEAT-0023 layer (REQ-MPROFILE-09) appends two **active-profile identity**
  string columns to the standard control-loop CSV: `active_profile_name` (the
  resolved profile name driving this run) and `active_profile_source` (how that
  profile was selected — one of `explicit_config`, `profile_flag`, `profile_env`,
  `machine_identity`, or `default`). Both are written verbatim as the last two
  fields of every control-loop row and are not blanked. They are appended only to
  the control-loop CSV (outside `BuildCommonCsvHeader`), so the read-loop and
  foreground `evidence-log` CSVs are unchanged. These are additive evidence
  columns only: they are **not** ingested by `analyze` (no analyzer schema bump),
  and they are never a response source, write gate, breaker input, or fan-duty
  input. The same `active_profile_name`/`active_profile_source` identity is also
  mirrored into the worker `control_runtime.json` status payload.
- Status publication is rate-limited in the current implementation, so tools
  must not assume `control_runtime.json` updates every tick.
- Sensor-failure and circuit-breaker state is exposed in
  `control_runtime.json` as explicit per-channel fields and transition events.
  `--reset-breakers` provides the deliberate operator/runtime path for clearing
  open write-failure breakers without process restart.
- Decision records are generated automatically by the analyzer for Markdown
  summaries. The operator still needs to fill in the final result/follow-up once
  the before/after comparison is known.

## Evaluation Workflow

Use this loop for controller changes:

1. State the hypothesis before changing config. Example: "raising channel 4
   thermal-pressure max boost by 3% will reduce combined-load CPU/Tctl p90
   without increasing idle writes."
2. Capture a run label and notes: workload, start/stop times, ambient if known,
   subjective noise notes, active config path, build, and git commit.
3. Run one fixed profile at a time:
   - idle hold,
   - GPU step,
   - combined CPU plus GPU,
   - CPU-only diagnostic only when needed,
   - cooldown.
4. Collect the native runtime manifest first, then the active CSV archive,
   `control_runtime.json`, `current_state.json`, and
   `svg_mb_control_events.jsonl`.
5. Summarize the run before tuning. Use the native analyzer as the default first
   pass:
   ```powershell
   release\svg-mb-control.exe analyze ingest `
     --runtime-home .\release\runtime `
     --db .\release\runtime\svg_mb_control.db
   release\svg-mb-control.exe analyze prune `
     --runtime-home .\release\runtime `
     --db .\release\runtime\svg_mb_control.db `
     --db-retain-days 30 `
     --dry-run
   release\svg-mb-control.exe analyze report `
     --runtime-home .\release\runtime `
     --db .\release\runtime\svg_mb_control.db `
     --idle-seconds 300 `
     --profile combined-load `
     --notes "ambient and subjective noise notes" `
     --out run-summary.txt `
     --manifest-out run-manifest.json
   ```
   This also writes `run-summary.decision.md` automatically. The decision
   record includes artifact hashes, run identity, channel response attribution
   counts, event counts, and automatic flags for hot-but-low/no-response runs.
   Override the path with `--decision-record-out <path>` or suppress it with
   `--no-decision-record`.
   The summary should cover at least:
   - idle/load/cooldown CPU/Tctl p50, p90, max,
   - idle/load/cooldown GPU memory junction and derived GPU envelope p50, p90,
     max,
   - GPU envelope peak timing, optional threshold time, and channel setpoints
     at the GPU peak,
   - achieved interval p50, p90, p95, p99, max, average, and overrun count,
   - loop work duration p50, p90, p95, p99, max, average,
   - process CPU, working-set, and private-byte percentiles,
   - write-count delta by channel,
   - setpoint p50, p90, max by channel,
   - primary source counts by channel (`cpu`, `gpu`, CPU telemetry fallback,
     guard fallback, and unavailable),
   - max and saturation time for thermal-pressure boost,
   - downward setpoint steps above the configured deadband,
   - authority reassertions, policy refusals, write failures, sensor failures,
     and circuit-breaker events,
   - recovery time back to near-idle setpoints after load stops.
   Also check `docs\CONTROL_PIPELINE_MATH.md` against the run's CSV/status
   identities: feedforward/correction math, low-band effective-cap behavior,
   cadence bounds, setpoint bounds, and response-source attribution.
   For CPU/AIO tuning questions where the native report is too broad, extract
   radiator-focused segments from the same CSV:
   ```powershell
   python .\scripts\extract_cpu_aio_segments.py `
     --runtime-home .\release\runtime `
     --out-dir .\release\runtime\analysis
   ```
   Review the segment index first, then the per-segment trace CSVs. The index
   separates radiator channels `1/4/5` from context fans `0/2/3` and flags the
   first GPU confound so CPU-only radiator magnitude is not overstated. Use
   `--csv <archive.csv>` instead of `--runtime-home` when a comparison needs to
   be pinned to a closed archive rather than the moving live mirror.
   For CPU temperature comparisons after FEAT-0020 power logging, also run the
   package-power view:
   ```powershell
   python .\scripts\analyze_cpu_temp_power.py `
     --runtime-home .\release\runtime `
     --machine-policy .\config\machines\snd-desk.cooling.policy.json `
     --ambient-c 21 `
     --out .\release\runtime\analysis\cpu-temp-power-latest.md `
     --json-out .\release\runtime\analysis\cpu-temp-power-latest.json `
     --window-csv-out .\release\runtime\analysis\cpu-temp-power-latest-windows.csv
   ```
   Compare same package-power band, ambient, GPU confound state, and radiator
   setpoint/RPM context. Use the older busy-band ledger for long-term trend
   continuity, not as the final arbiter once package watts are available.
6. Change one class of knob at a time:
   - curve breakpoints,
   - thermal-pressure boost,
   - fall/release behavior,
   - deadband/cooldown,
   - channel membership.
7. Review the generated decision record, set the final decision/result, and
   record the exact config fields changed for the before/after comparison.

## Tuning Guidance

Do not jump to PID as the next controller model. The current controller is
already a practical nonlinear control stack:

- gain-scheduled feed-forward curves,
- CPU overlay on top of GPU envelope,
- asymmetric first-order demand smoothing,
- bounded slow integral trim through `thermal_pressure_*`,
- final slew limiting through rise/fall rates,
- deadband and cooldown quantization before writes.

Tune that model from data first:

- If high-load CPU/Tctl remains too high, raise the relevant CPU overlay point
  or the radiator thermal-pressure max boost. Prefer channels `4` and `5`
  before channel `1` unless the run shows otherwise.
- If GPU memory is high but CPU is acceptable, tune the GPU envelope curves for
  front-intake and case-airflow lanes before raising CPU overlay.
- If fans release too slowly after load, raise
  `thermal_pressure_fall_pct_per_sec` in small steps.
- If downward hot-zone steps are audible, lower the channel deadband or reduce
  fall alpha on that channel before changing rise behavior.
- Keep off-floor rise behavior intact unless a measured run proves it is too
  abrupt or too noisy.

PID status, 2026-06-22: FEAT-0003's seam and shadow/dry-run PID are implemented,
and the first real-archive replay
(`docs/pid-shadow-characterization-2026-06-21.md`) still rejects all-channel
live PID. The channel-0-only live gate passed in
`docs/pid-live-channel0-evidence-2026-06-22.md` with the existing
`pid.allow_live` + characterization-artifact + positive-slew-cap gate; the
shipped default remains `curve_overlay`.

`docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` records the rationale, hardware basis,
and re-validation procedure for the adopted low-load airflow policy in
`config\control.release.json`, including the dynamic low/medium intake curves.
Treat that document as the worked example of a tuning decision record and
update it (or write a sibling note) when the shipped floor or low-end curve
profile is intentionally changed again.

## Next Actions

1. Reuse ideas from the GPU programs where they fit: segment accounting and
   deadline/cadence summaries. Compact decision records are now generated by
   the analyzer; keep them small enough to commit when they justify a tuning
   change. Do not import tray UI, service orchestration, or external metrics
   infrastructure until the Control schema stabilizes.
