# Discovery — Logging Parity

**Goal:** Evaluate the current `SVG-MB-Control` runtime state/logging surface, infer the intended direct-runtime plan, inspect `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl`, and determine what logging approach should be borrowed.
**Date:** 2026-04-16
**Status:** complete
**Recommended next:** none — standalone research; the logging change is scoped enough to implement directly without a larger campaign

---

## Questions

1. What logging or state-publication surface already exists in `SVG-MB-Control`?
2. What does the current repo appear to be planning or optimizing for?
3. How does `NVG_SmoothControl` logging actually work?
4. Which `NVG_SmoothControl` patterns are worth copying into `SVG-MB-Control`?
5. What concrete logging shape best fits `SVG-MB-Control`?

---

## Findings

### Q1: What logging or state-publication surface already exists in `SVG-MB-Control`?

**Answer:** `SVG-MB-Control` currently has a live state plane, not a historical logging plane. It persistently writes structured runtime JSON (`current_state.json`, `control_runtime.json`, `pending_writes.json`) and emits failures to `stderr`, but it does not maintain append-only log files, log rotation, archive indexing, or an ingest pipeline.

**Evidence:**
- `README.md:17-20` defines repo-owned runtime files under `runtime\`.
- `README.md:142-146` lists only `current_state.json`, `control_runtime.json`, and `pending_writes.json` as runtime outputs.
- `docs/RUNTIME_HOME.md:13-17` defines the same three files as Control-owned runtime artifacts.
- `src/read_loop.cpp:87-127` atomically writes `control_runtime.json`.
- `src/read_loop.cpp:215-253` writes `current_state.json`, optionally mirrors it to `snapshot_path`, and updates `control_runtime.json`.
- `src/control_loop.cpp:425-474` writes `control_runtime.json` for control-loop status.
- `src/control_loop.cpp:543-545` republishes `current_state.json` from the direct runtime snapshot each control-loop tick.
- `src/write_orchestrator.cpp:99-280` reports validation, write, restore, and reconciliation failures via `std::cerr`.
- `src/main.cpp:384-392` writes one-shot snapshot JSON to `stdout` and fatal errors to `stderr`.

**Implications:**
- The repo already has authoritative live machine-readable state.
- What is missing is durable history: per-tick telemetry archives, write-event history, and operator-visible session records.

### Q2: What does the current repo appear to be planning or optimizing for?

**Answer:** The repo is converging on a standalone, direct-only runtime where `svg-mb-control.exe` owns telemetry sampling, write orchestration, recovery, and runtime-home publication. Bridge-era supervision and logger-service assumptions were intentionally removed, and logging was likely deferred while the direct runtime contract was established.

**Evidence:**
- `README.md:3-6` states the repo is standalone and must not depend on sibling repos at runtime.
- `README.md:12-18` defines the scope as direct AMD telemetry, optional vendored GPU telemetry, direct fan reads/writes, and product-owned runtime files.
- `README.md:127-129` explicitly rejects legacy bridge-era config fields including `logger_service_duration_ms`.
- `src/control_config.cpp:250-270` hard-fails on legacy fields such as `bridge_exe_path`, `bench_runtime_policy_path`, and `logger_service_duration_ms`.
- `src/main.cpp:198-204` rejects legacy bridge CLI options and states that this branch runs direct-only.
- `docs/RUNTIME_HOME.md:99-105` makes Control the only writer of runtime-home files.

**Implications:**
- The intended architecture is single-product ownership of runtime behavior and state.
- Any logging design that fits this repo should stay in-process and repo-owned rather than reviving an external logger service.

### Q3: How does `NVG_SmoothControl` logging actually work?

**Answer:** `NVG_SmoothControl` uses a layered logging architecture:

1. a high-frequency CSV time-series archive for controller ticks,
2. a fixed-path live mirror for compatibility readers,
3. a machine-readable runtime status snapshot,
4. a command file for stop/log-rotation requests,
5. shared memory for low-latency status consumers,
6. `.ready` markers plus a named Windows event for archive lifecycle notifications,
7. native indexing of closed sessions into JSONL, and
8. offline ingest into SQLite.

**Evidence:**
- `README.md:133-144` defines `runtime\logs\nvg_control_status.json`, `runtime\logs\nvg_control_command.json`, archived CSV chunks, a fixed live CSV mirror, a session index JSONL, and a SQLite database.
- `README.md:144` says the active archive path is published as `logCsvPath` in runtime status and shared memory, and that closed chunks create `.ready` markers and signal `NVG_SmoothControl_LogReady`.
- `CONFIG_REFERENCE.md:45-49` documents status, command, archive, and live-output files.
- `CONFIG_REFERENCE.md:64-64` documents the `.ready` marker plus named-event ingestion handshake.
- `CONFIG_REFERENCE.md:72-73` exposes runtime-configured log rotation and retention.
- `CONFIG_REFERENCE.md:77-79` states that CSV logging records every controller tick by default.
- `CONFIG_REFERENCE.md:97-101` documents inline campaign annotations written into the active CSV stream.
- `src/csv_logger.h:25-34` describes archive chunks, live mirror semantics, and `.ready` markers.
- `src/csv_logger.h:186-267` implements rotation and retention pruning.
- `src/runtime_control.h:23-75` defines status and command paths.
- `src/runtime_control.h:184-201` writes command JSON atomically.
- `src/runtime_control.h:260-327` writes the runtime status JSON atomically, including `logCsvPath`.
- `src/shared_data.h:25-34` defines the shared-memory and log-ready event names.
- `src/main.cpp:805-833` opens the CSV logger, writes metadata/header, configures rotation, and creates the log-ready event.
- `src/main.cpp:959-1048` publishes live status into shared memory and JSON.
- `src/main.cpp:1077-1109` consumes runtime commands, including log rotation.
- `src/main.cpp:1255-1325` writes per-tick CSV rows and optionally mirrors them to `stdout`.
- `src/tray/supervisor.cpp:1063-1114` consumes `.ready` markers into a native session index.
- `src/ingest/ingest_csv.cpp:644-676` discovers candidate log roots.
- `src/ingest/ingest_csv.cpp:681-717` skips live runtime logs and ingests only closed/archive data.

**Implications:**
- `NVG_SmoothControl` treats logging as a product subsystem, not as incidental `printf`.
- The important architectural split is between live status, live command/control, archival telemetry, and offline analysis.

### Q4: Which `NVG_SmoothControl` patterns are worth copying into `SVG-MB-Control`?

**Answer:** The first-tier patterns are worth copying; the full ingest stack is not yet necessary.

**Copy now:**
- a dedicated `runtime\logs\` area distinct from the authoritative live state files,
- append-only per-session telemetry archives,
- a stable live-output path for tools that want to tail current activity,
- explicit log rotation and retention controls,
- publication of the active log path in runtime status,
- structured inline event markers for meaningful control actions.

**Copy later only if needed:**
- a command file for runtime log rotation,
- shared memory for low-latency UI readers,
- `.ready` markers plus a session index,
- offline ingest into SQLite.

**Evidence:**
- `docs/RUNTIME_HOME.md:19-80` shows that `SVG-MB-Control` already has a stable live-state schema, so it does not need NVG’s status snapshot concept copied verbatim.
- `src/read_loop.cpp:215-253` and `src/control_loop.cpp:520-545` already publish the current machine state every tick.
- `src/write_orchestrator.cpp:156-225` already has distinct lifecycle points that would benefit from durable event logging: baseline capture, sidecar upsert, apply duty, restore, sidecar removal.
- `CONFIG_REFERENCE.md:72-79` and `src/csv_logger.h:186-267` show that rotation/retention are cheap, self-contained features that do not require the full tray/analyzer stack.
- `README.md:144` and `src/runtime_control.h:274-275` show the value of publishing the current active log path to consumers.

**Implications:**
- `SVG-MB-Control` should borrow NVG’s separation of live state vs. archived time series.
- It should not immediately borrow NVG’s service/tray/shared-memory/ingest complexity unless a second consumer actually exists.

### Q5: What concrete logging shape best fits `SVG-MB-Control`?

**Answer:** The best fit is a two-plane design:

1. keep `current_state.json`, `control_runtime.json`, and `pending_writes.json` as the authoritative live state/recovery plane, and
2. add a new `runtime\logs\` telemetry plane for append-only history.

The logging plane should start with:

- `runtime\logs\archive\svg_mb_control_<timestamp>.csv` as the immutable session archive,
- `runtime\logs\svg_mb_control_output.csv` as the fixed-path live mirror,
- optional `runtime\logs\svg_mb_control_events.jsonl` or CSV comment markers for write/restore/failure events,
- `control_runtime.json` extended with active log path metadata.

Recommended initial CSV columns:

- `wall_clock`
- `mode`
- `snapshot_time`
- CPU fields such as `cpu_tctl_c` and maybe `cpu_max_c`
- GPU fields such as `gpu_core_c`, `gpu_memjn_c`, `gpu_hotspot_c`
- per-channel fan metrics for the fixed supported motherboard channels
- `policy_writes_enabled`
- `loop_tick_count` when in control-loop
- `channel_write_count_total_<n>` or compact per-channel duty/setpoint columns
- high-value control outputs such as chosen temperature, setpoint, deadband skip, cooldown skip, write applied, restore applied

Recommended initial event coverage:

- control-loop start/shutdown,
- baseline captured per channel,
- write applied,
- write skipped due to policy/deadband/cooldown,
- restore succeeded/failed,
- reconciliation restored/failed,
- reader init warnings.

**Evidence:**
- `src/control_loop.cpp:495-512` already assembles rich loop metadata suitable for session header logging.
- `src/control_loop.cpp:547-657` already computes the exact control decisions that should become structured log events.
- `src/write_orchestrator.cpp:156-225` already exposes the write lifecycle in a way that maps directly to durable event records.
- `src/main.cpp:205-246` already provides operator-oriented diagnostic modes for AMD/GPU readers that could share the same formatting helpers.
- `src/csv_logger.h:269-309` shows a practical pattern for metadata headers plus fixed columns without requiring a schema-heavy binary format.

**Implications:**
- A CSV-based time-series log is a good match for the read-loop/control-loop telemetry stream.
- A separate event stream or comment markers are needed so write/restore/reconciliation actions are not lost inside a dense tick log.

---

## Cross-Cutting Analysis

### Constraints
- `SVG-MB-Control` is direct-only and should not reintroduce an external logger or service dependency.
- The repo already relies on `runtime_home` JSON files for live control semantics; new logging should not replace them.
- Fan channels are board-specific but still bounded enough that a fixed-column CSV is viable if the supported channel set remains stable.

### Risks
| Risk | Likelihood | Impact | Notes |
|------|-----------|--------|-------|
| Copying the full NVG logging stack too early | M | M | Shared memory, rotate commands, session index, and SQLite ingest are valuable only once there is a second consumer. |
| Adding only CSV without event records | H | M | Per-tick telemetry alone will miss why writes or restores happened unless events are embedded or logged separately. |
| Replacing current runtime JSON with logs | L | H | `current_state.json` and `control_runtime.json` are already part of the control contract and should remain authoritative. |

### Open Questions
- Whether the preferred durable history format is one CSV plus inline event comments, or CSV plus a companion JSONL event file.
- Whether `read-loop` and `control-loop` should share one row schema, or use one superset schema with blank fields outside control-loop.

---

## Recommendation

Proceed with a repo-local logging subsystem modeled on the *first half* of `NVG_SmoothControl`:

- keep the existing runtime-home JSON files as the live state plane,
- add `runtime\logs\archive\...csv` plus `runtime\logs\svg_mb_control_output.csv`,
- add rotation/retention config,
- publish the active log path from `control_runtime.json`,
- add structured control/write events either as CSV comments or a companion JSONL file.

Do **not** start with shared memory, command files, session indexing, or SQLite ingest unless there is already a consumer that needs them.
