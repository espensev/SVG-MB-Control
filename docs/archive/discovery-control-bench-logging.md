# Discovery - Control vs Bench Logging

**Goal:** Evaluate next possible logging steps, compare Control logging against Bench logging, and assess whether the general logging layer can be improved outside the controller.
**Date:** 2026-05-16
**Status:** complete; superseded by later evidence-log and analyzer work
**Recommended next:** use `docs\RUNTIME_LOGGING_AND_EVALUATION.md` and
`docs\archive\implemented-plans\LOGGING_IMPROVEMENT_PLAN.md` for current workflow. The read-only
`evidence-log` plane, richer GPU/SIO/fan fields, decision records, and event
classification are implemented.

> Historical note, 2026-05-21: many file/line references below predate the
> runtime module split. Keep the findings as design history; use the maintained
> runtime logging docs for current paths and workflow.

---

## Questions

1. What does Control currently log in-process, and where is that implemented?
2. What does Bench currently log, and how is its logging split structurally?
3. What meaningful evidence fields does Bench capture that Control does not?
4. Which logging pieces are safe to generalize outside the controller without making Control depend on Bench at runtime?
5. What are the next implementation steps, ordered by value and risk?

---

## Findings

### Q1: What does Control currently log in-process?

**Answer:** Control now has native runtime logging that is good enough for controller operation and tuning. It writes per-tick CSV rows, JSONL events, current-state JSON, control status JSON, and runtime log manifests. The implementation is in Control itself and does not depend on Bench at runtime.

**Evidence:**
- `src/runtime_artifacts.h:53` - `RuntimeCsvLogger` is the Control-owned CSV logger API.
- `src/runtime_artifacts.cpp:184` - runtime manifests are written by `RuntimeCsvLogger::WriteManifest`.
- `src/runtime_artifacts.cpp:201` - manifests publish `session_stop` for terminal chunks and keep it null while running.
- `src/runtime_artifacts.cpp:206` - manifests publish `rows_written` compatibility accounting for finalized chunks.
- `src/runtime_artifacts.cpp:208` - manifests publish `events_written` compatibility accounting for finalized chunks.
- `src/runtime_artifacts.cpp:219` - manifests explicitly set `external_logging.required=false`.
- `src/runtime_artifacts.cpp` - manifest records the active CSV flush policy,
  flush interval, and latest-file mirror mode.
- `src/runtime_artifacts.cpp:382` - JSONL events are appended through `AppendRuntimeEvent`.
- `src/runtime_logging.cpp:121` - Control common CSV header starts with wall clock, mode, snapshot time, AMD summary, GPU thermal fields, fan state, and write-policy fields.
- `src/runtime_logging.cpp:279` - control-loop CSV header adds loop tick, wall-clock, timing, CPU, memory, and per-channel control fields.
- `src/control_loop.cpp:375` - control loop opens `RuntimeCsvLogger` directly.
- `src/control_loop.cpp:869` - control loop writes a CSV row every tick.
- `src/read_loop.cpp:152` - read loop also uses the same runtime CSV logger.
- `src/control_status_writer.cpp:93` - channel control state is projected into runtime log/status structures.
- `docs/RUNTIME_HOME.md:91` - `control_runtime.json` includes `loop_tick_count`.
- `docs/RUNTIME_HOME.md:100` - `control_runtime.json` includes `process_cpu_delta_ms`.
- `docs/RUNTIME_HOME.md:101` - `control_runtime.json` includes `process_cpu_pct`.

**Implications:**
- The controller no longer needs a sidecar Bench logger for normal control evidence.
- Control's logger is intentionally narrow: it focuses on runtime state, control decisions, restore/write failures, and tuning fields.
- The product-specific schema builders and general artifact plumbing are now split between `runtime_logging.*` and `runtime_artifacts.*`.

### Q2: What does Bench currently log, and how is its logging split structurally?

**Answer:** Bench has a richer standalone logger-service. It is structured as a measurement pipeline: config, runtime bootstrap, per-iteration capture, typed iteration state, CSV/current-state assembly, live heartbeat/stats, segment artifact creation, lifecycle, status reader, and shared artifact writers/manifests.

**Evidence:**
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\README.md:55` - `src/logger/` contains the standalone `logger-service`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\README.md:58` - shared support includes artifact paths/writers, manifests, hardware adapters, and runtime policy.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\README.md:368` - logger-service combines AMD SMN, Super I/O, and GPU telemetry, rotates segments, refreshes `current_state.json`, and keeps latest CSV/JSONL live.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\README.md:377` - latest CSV, JSONL, manifest, and current-state paths are convenience surfaces; archive paths remain the comparison contract.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:15` - `logger_runtime.*` owns backend bootstrap and current-state backend metadata.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:16` - `logger_capture.*` owns per-iteration AMD, SIO, and GPU reads.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:17` - `logger_state.*` owns typed iteration snapshots, CSV rows, and current-state updates.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:18` - `logger_live.*` owns heartbeat, EWMA stats, and recent-event state.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:19` - `logger_segment.*` owns segment artifact creation and manifest parameters.

**Implications:**
- Bench has the cleaner general logging architecture.
- The reusable parts are not the controller logic; they are artifact paths, artifact writers, session manifests, event accounting, segment lifecycle, and typed row assembly patterns.
- Control should not take a runtime dependency on the sibling Bench repo. AGENTS.md requires Control to stay standalone.

### Q3: What meaningful evidence fields does Bench capture that Control does not?

**Answer:** Bench captures a more complete hardware evidence set. The largest gaps are GPU non-thermal telemetry, Super I/O voltages and temperatures, fan raw byte decomposition, change flags, per-backend read cost, logger live statistics, and richer finalized manifest accounting.

**Evidence:**
- `src/runtime_snapshot.h:30` - Control `RuntimeGpuSnapshot` contains only availability/name/warning plus `core_c`, `memjn_c`, and `hotspot_c`.
- `src/gpu_reader.cpp:52` - Control samples GPU with `GpuSampleMode::ThermalFast`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:32` - Bench has a `LoggerGpuSample` structure.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:52` - Bench logs GPU utilization.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:57` - Bench logs GPU power.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:66` - Bench logs VRAM usage.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:70` - Bench logs PCIe throughput.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:75` - Bench logs core voltage.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:76` - Bench logs throttle reasons.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.cpp:63` - Bench normalizes selectable GPU sample modes.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.cpp:181` - Bench samples through the selected GPU sample mode.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_state.h:69` - Bench tracks fan RPM change flags.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_state.h:72` - Bench tracks fan duty change flags.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_state.h:77` - Bench keeps `tach_hi_raw`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_state.h:78` - Bench keeps `tach_lo_raw`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_capture.h:17` - Bench capture artifacts include SIO voltage states.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_capture.h:18` - Bench capture artifacts include SIO temperature states.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:304` - logger-service rows include capture/read timing fields.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:306` - logger-service SIO fan columns include change flags and raw tach bytes.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:307` - logger-service can include SIO voltage columns.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:308` - logger-service can include SIO temperature columns.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:309` - logger-service GPU columns include read cost, thermal valid/change flags, clocks, P-state, utilization, power, fans, VRAM, PCIe, voltage, and throttle reasons.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\session_manifest.h:35` - Bench manifests include `rows_written`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\session_manifest.h:36` - Bench manifests include `events_written`.

**Implications:**
- Control is missing evidence useful for offline characterization and AI/analytics datasets.
- Most of these fields are not necessary in the real-time control loop every 50 ms.
- A read-only evidence logger can capture the full set without increasing write-loop complexity or control latency.

### Q4: Which pieces are safe to generalize outside the controller?

**Answer:** The safe generalization target is a Control-owned logging/artifact substrate plus a separate read-only evidence logger. Control already has the hardware capability in its vendored dependencies for several Bench-only fields, so it can stay standalone.

**Evidence:**
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_writers.h:70` - Bench has a reusable `CsvLogWriter`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_writers.cpp:185` - `CsvLogWriter::write_row` handles row writes.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_writers.cpp:251` - `CsvLogWriter::finalize` owns final latest/archive synchronization.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_writers.h:103` - Bench has a JSONL writer with latest/archive support.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_paths.h:9` - artifact path data is centralized in `ArtifactPaths`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_paths.cpp:158` - archive CSV path is constructed centrally.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\artifact_paths.cpp:163` - latest CSV path is constructed centrally.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\session_manifest.cpp:100` - `SessionManifest::start` writes a starting manifest.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\session_manifest.cpp:135` - `SessionManifest::finish` finalizes rows/events/status.
- `third_party\SVG-MB-SIO\include\svg_mb_sio\svg_mb_sio.h:42` - Control's vendored SIO API exposes voltage snapshots.
- `third_party\SVG-MB-SIO\include\svg_mb_sio\svg_mb_sio.h:49` - Control's vendored SIO API exposes SIO temperature snapshots.
- `third_party\SVG-MB-SIO\include\svg_mb_sio\svg_mb_sio.h:131` - Control's vendored SIO API exposes `read_voltages`.
- `third_party\SVG-MB-SIO\include\svg_mb_sio\svg_mb_sio.h:134` - Control's vendored SIO API exposes `read_temperatures`.
- `third_party\nvapi-controller\telemetry\include\gpu_telemetry\gpu_snapshot.h:84` - Control's vendored GPU snapshot has VRAM fields.
- `third_party\nvapi-controller\telemetry\include\gpu_telemetry\gpu_snapshot.h:96` - Control's vendored GPU snapshot has NVML power.
- `third_party\nvapi-controller\telemetry\include\gpu_telemetry\gpu_snapshot.h:113` - Control's vendored GPU snapshot has PCIe throughput.
- `third_party\nvapi-controller\telemetry\include\gpu_telemetry\gpu_snapshot.h:119` - Control's vendored GPU snapshot has throttle reasons.

**Implications:**
- A general logger can be added inside Control without reintroducing sibling-repo runtime dependencies.
- The first reusable primitive now exists as a Control-owned artifact layer with product-specific schema builders above it.
- The control loop should continue logging the fields needed for real-time control and recovery. Richer evidence capture should run in a separate read-only mode or command.

### Q5: What are the next implementation steps?

**Historical answer:** Do this in stages. First extract general artifact
plumbing, then add a read-only evidence logger, then broaden field coverage.
That staged plan has since landed. The durable guidance is still to avoid
putting the entire Bench logger into the active control loop.

**Evidence:**
- `src/runtime_artifacts.cpp:184` - Control manifests are now isolated in the artifact layer.
- `src/runtime_logging.cpp:279` - control-loop schema assembly remains product-specific.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:16` - Bench separates capture from row assembly.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:17` - Bench separates typed iteration state from row/current-state assembly.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\README.md:19` - Bench separates segment artifact creation and manifest parameters.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:290` - Bench documents a buffered flush window for lower hot-path I/O cost.

**Implications:**
- Stage 1 has been implemented as a low-risk refactor with no CSV schema change and with additional manifest compatibility fields.
- Stage 2 can add a new command or mode without changing control-loop behavior.
- Stage 3 can expand fields once the independent logger has a stable contract.

---

## Cross-Cutting Analysis

### Constraints

- Control must stay standalone. Do not make runtime behavior depend on `SVG-MB-Bench` or sibling repo paths.
- The active controller should not inherit Bench's full measurement cadence/field cost on the 50 ms control path.
- Latest files are single-session convenience surfaces. Archive files plus manifests should be the durable comparison contract.
- Field expansion should be versioned. Control's current CSV schema is `svg_mb_control.log.v1`; richer evidence rows need either a new schema or a distinct command schema.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Moving rich GPU/SIO capture into the control loop increases latency | Medium | High | Full GPU and SIO voltage/temp reads are measurement-grade, not always control-grade. |
| Copying Bench code wholesale creates sibling-repo drift | High | Medium | Better to port patterns or extract a small Control-owned substrate. |
| Changing Control's current CSV schema breaks analyzer tests | Medium | Medium | Add new columns carefully or create a separate evidence schema. |
| Per-row write-through remains expensive at high evidence cadence | Medium | Medium | Bench already has buffered writes with final archive/latest synchronization; use that pattern for the separate evidence logger rather than the control loop. |
| Concurrent logger/controller writes collide on latest paths | Medium | Medium | Use separate command tokens/latest paths and shared archive conventions. |

### Open Questions

- Whether the new read-only evidence logger should live as `svg-mb-control.exe logger-service`, `svg-mb-control.exe evidence-log`, or a separate executable target.
- Whether Control should adopt Bench-compatible manifest schema version `2`, or keep a Control-specific manifest schema after adding `rows_written`/`events_written` compatibility fields.
- Whether the first rich logger should sample GPU in `full`, `medium`, or `thermal-fast` mode by default.

---

## Recommendation

This recommendation has been implemented. The read-only evidence logger now
uses separate artifact names under the Control runtime home, captures richer
SIO/fan/GPU evidence, supports GPU sample modes, and reports per-backend timing
outside the controller hot path.

Historical implementation order:

1. Done: extract Control's generic artifact path/writer/manifest code out of `runtime_logging.cpp` into a small Control-owned logging core. Preserve current control/read-loop behavior.
2. Done: add manifest compatibility fields: `rows_written`, `events_written`, `session_stop`, and terminal row accounting alongside the existing Control manifest fields.
3. Done: add a read-only evidence logger outside the control loop. It produces
   its own archive CSV, latest CSV, JSONL events, manifest, and current-state
   JSON.
4. Done: expand evidence capture with fan raw byte split/change flags, SIO
   voltage/temp arrays, selectable GPU sample modes, and per-backend timing.
5. Still current: mirror a subset into the controller's per-tick CSV only when
   control-loop evidence proves the hot path needs it.

Do not make the controller the "complete logger." Make Control own two planes:

- controller plane: narrow, deterministic, write/recovery/control evidence
- evidence plane: read-only, complete hardware telemetry for characterization and analysis
