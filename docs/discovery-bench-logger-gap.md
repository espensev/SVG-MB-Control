# Discovery - Bench Logger Gap

**Goal:** Evaluate the updated SVG-MB-Bench logger against SVG-MB-Control so Control can stop relying on external loggers.
**Date:** 2026-05-14
**Status:** complete; superseded by later Control evidence-log work
**Recommended next:** no remaining Bench dependency work. Control now owns the
normal validation logger plus a foreground `evidence-log` plane with richer
GPU/SIO/fan evidence. Keep Bench as an optional characterization reference, not
a runtime dependency.

> Historical note, 2026-05-21: the gap analysis below predates the Control
> evidence-log widening. Statements about missing Control evidence are retained
> as original context, not current status.

---

## Questions

1. What does the updated SVG-MB-Bench logger capture that matters for replacing external loggers?
2. What artifact contract does Bench enforce around evidence quality?
3. What does SVG-MB-Control already log internally?
4. What gaps remain before Control can be treated as the sole logging/evidence source?

---

## Findings

### Q1: What does the updated SVG-MB-Bench logger capture?

**Answer:** Bench now captures a wider hardware evidence surface than Control: full GPU telemetry, Super I/O voltages, Super I/O temperatures, fan raw count bytes, cadence/slip fields, current state, latest mirrors, archive segments, and manifests.

**Evidence:**
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:32` - `LoggerGpuSample` includes full GPU state, not only temperatures.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:44` - GPU sample includes `nvml_temp_c`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:52` - GPU sample includes utilization.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:57` - GPU sample includes power.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:61` - GPU sample includes GPU fan count and fan samples.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:64` - GPU sample includes VRAM fields.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:68` - GPU sample includes PCIe throughput/link fields.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:73` - GPU sample includes core voltage.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:74` - GPU sample includes throttle reasons.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\state_snapshot.h:150` - current state includes SIO voltages.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\state_snapshot.h:151` - current state includes SIO temperatures.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:73` - logger appends GPU CSV headers.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:185` - manifests record whether GPU capture was requested.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:186` - manifests record whether GPU capture is active.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:191` - manifests record GPU sample mode.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:202` - manifests record external writers.

**Implications:**
- Control should copy the capability model, not call into Bench.
- The most valuable deltas are richer GPU telemetry and board/SIO voltage-temperature evidence.

### Q2: What artifact contract does Bench enforce?

**Answer:** Bench treats a run as a first-class evidence bundle: archive CSV, JSONL events, manifest, current state, latest mirrors, row/event accounting, explicit external-writer accounting, cadence/slip fields, and raw register evidence.

**Evidence:**
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:7` - manifests expose `rows_written` and `events_written`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:99` - manifests include `external_writers`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:165` - fan fields include `tach_hi_raw` and `tach_lo_raw`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:197` - passive capture includes cadence and scheduler slip.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:200` - thermal logs retain SIO voltage readings.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:201` - thermal logs retain SIO temperature readings.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:202` - logs can preserve operator phase transitions.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:288` - `logger_service` combines AMD, SIO, and GPU telemetry.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:290` - latest CSV and JSONL are refreshed during capture, then replaced from closed archive artifacts.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:297` - `logger_service` records `capture_read_ms`, backend read times, wait lateness, skipped periods, and timer/spin wait usage.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:302` - GPU logger columns include validity/change flags, power, fans, VRAM, PCIe, voltage, and throttle reasons.

**Implications:**
- Original finding: Control's logging was useful but incomplete as a standalone
  evidence contract.
- Current status: run-native manifests and richer hardware rows now exist in
  Control; phase/load markers remain optional future context.

### Q3: What does Control already log?

**Answer:** Control now has a solid internal control-loop logging base: archive CSV, latest CSV mirror, JSONL events, native runtime manifests, current state, control runtime status, pending write recovery, control setpoints, timing, process resource fields, and a post-run analyzer.

**Evidence:**
- `src\runtime_logging.cpp:151` - Control builds a common CSV header.
- `src\runtime_logging.cpp:156` - Control logs GPU availability, name, and warning.
- `src\runtime_logging.cpp:157` - Control logs GPU core, memory junction, and hotspot temperatures.
- `src\runtime_logging.cpp:165` - Control logs fan tach count.
- `src\runtime_logging.cpp:167` - Control logs fan duty raw.
- `src\runtime_logging.cpp:169` - Control logs fan mode raw.
- `src\runtime_logging.cpp:497` - Control builds a control-loop CSV header.
- `src\runtime_logging.cpp:506` - Control logs loop slip.
- `src\runtime_logging.cpp:509` - Control logs process CPU percent.
- `src\runtime_logging.cpp` - Control writes `svg_mb_control.runtime_log_manifest.v1` manifests beside runtime CSV artifacts and at the fixed latest path.
- `scripts\analyze_control_run.py:399` - Control has post-run manifest generation.
- `scripts\analyze_control_run.py:424` - Control's analysis manifest records row count.
- `scripts\analyze_control_run.py:425` - Control's analysis manifest records event count.
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md:35` - `current_state.json` is the current telemetry snapshot.
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md:36` - `control_runtime.json` is the mode/status publication.
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md:39` - `pending_writes.json` is the recovery sidecar.
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md:48` - `control_runtime.json` is intentionally status, not per-tick data.

**Implications:**
- Control is already enough for control-loop tuning and operational visibility.
- Current status: Control is now enough for normal validation and richer
  foreground evidence capture. Bench remains useful for specialized comparison,
  not as a required external logger.

### Q4: What gaps remained at the time?

**Answer:** Four main gaps remained in the original snapshot. The first three
have since landed in Control's foreground evidence plane; phase/load context is
still deferred until run summaries show a need.

**Evidence and implications:**
- Full GPU evidence was missing. Control samples only `core_c`, `memjn_c`, `hotspot_c`, and GPU name/warning (`src\gpu_reader.h:7`, `src\gpu_reader.cpp:45`). Bench records NVML temp, utilization, power/source, clocks, P-state, GPU fan rows, VRAM, PCIe, voltage, throttle reasons, validity flags, and change flags (`D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_gpu.h:32`). Current Control evidence-log rows now cover the richer GPU set.
- SIO voltage and motherboard temperature evidence was missing. Bench current state includes voltage and temperature arrays (`D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\state_snapshot.h:150`, `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\state_snapshot.h:151`); Control then recorded AMD sensors and fans plus narrow GPU fields. Current Control evidence-log rows now include SIO voltage/temperature evidence.
- Raw fan count-byte evidence and change flags were missing. Control logs `tach_raw`, `duty_raw`, and `mode_raw` (`src\runtime_logging.cpp:165`, `src\runtime_logging.cpp:167`, `src\runtime_logging.cpp:169`), but Bench contract preserves `tach_hi_raw`, `tach_lo_raw`, rpm/duty changed flags, and full field semantics (`D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:299`). Current Control evidence-log rows now include tach high/low bytes and change flags.
- Phase/load/external-writer context is incomplete. Bench contract explicitly preserves phase transitions and load state (`D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\LOG_ARTIFACT_CONTRACT.md:202`), plus external writer accounting (`D:\Development\Thermals\SVG-MB\SVG-MB-Bench\src\logger\logger_segment.cpp:202`).

---

## Cross-Cutting Analysis

### Constraints

- Control must remain standalone; do not reintroduce runtime dependency on SVG-MB-Bench.
- The live controller must keep `current_state.json`, `control_runtime.json`, and `pending_writes.json` as the authoritative live/recovery plane.
- Extra logging cannot destabilize the 50 ms control-loop cadence.

### Risks

| Risk | Likelihood | Impact | Notes |
| --- | --- | --- | --- |
| Logging hot-path cost regresses control cadence | Medium | High | Bench records backend read and scheduler timing; Control should do the same before enabling heavier fields by default. |
| GPU full sampling is too expensive for every 50 ms control tick | Medium | Medium | Bench exposes sample modes; Control should use tiered GPU sampling or split control envelope from diagnostic capture. |
| Adding SIO voltage/temp reads under the control loop increases mutex contention | Medium | Medium | Prefer optional lower-rate side capture or measured cadence gate. |
| Native manifests lack artifact hashes today | Medium | Medium | The controller now writes row/event counts and artifact paths during the run; hash capture still belongs in a finalize/analysis pass. |

### Open Questions

- Whether full GPU sampling can run inside Control's 50 ms loop without measurable slip on this system.
- Whether SIO voltage/temp reads should be sampled every tick or at a slower diagnostic cadence.
- Which board temperature sensors are required to replace the current external reference logger for day-to-day tuning.

---

## Recommendation

### Bench Reorg Export Map

Bench's updated `src\logger\` layout is a good staging boundary for Control work. Treat it as a source package to harvest concepts from, not as a runtime dependency or wholesale import.

**Export directly or adapt with minimal changes:**

1. Artifact writer behavior: buffered CSV flushing, explicit flush/finalize statistics, latest-file mirror semantics, and archive/current artifact accounting. Control should make the flush window explicit because Bench's 10-row buffer improves hot-path cost but permits a bounded loss window on hard crash.
2. Manifest/lifecycle semantics: startup publication, running segment manifests, rollover, completed/failed finalize paths, row/event counts, writer stats, and terminal state.
3. GPU telemetry model: richer sample result fields, sample modes, validity/change flags, per-read timing, and reusable sample buffers. Keep Control's default mode lightweight.
4. Snapshot schema ideas: GPU, voltage, motherboard temperature, and fan evidence fields that can be written to CSV/current-state consistently.
5. Offline test patterns: manifest correctness, latest artifact surfaces, disabled-backend states, lifecycle failure/finalize coverage, and performance assertions around row-write/flush timing.

**Do not export as-is:**

1. Bench command-line/config parsing. Control should expose these through its own runtime configuration and mode flags.
2. Bench `logger_core` orchestration loop. Control already owns `read-loop` and `control-loop`; the logger should become an artifact layer under those loops.
3. Bench probe/campaign dependencies. Control must stay standalone and should only import hardware-reader concepts that are needed for runtime evidence.

**Suggested Control target shape:**

```text
src\logging\artifact_paths.*
src\logging\artifact_writers.*
src\logging\session_manifest.*
src\logging\runtime_log_context.*
src\logging\runtime_csv_schema.*
src\logging\runtime_event_log.*
src\logging\runtime_state_snapshot.*
src\logging\logger_lifecycle.*
src\hardware\gpu_telemetry.*
src\hardware\sio_snapshot.*
```

If a smaller first step is preferred, keep the current flat style and introduce only `runtime_artifacts.*`, `runtime_manifest.*`, `gpu_telemetry_reader.*`, and `sio_board_snapshot.*`, then move them under `src\logging\` / `src\hardware\` once the interfaces settle.

Historical implementation order:

1. Done: extend native runtime manifests with finalized accounting and
   config/policy hashes needed for standalone archival.
2. Done: port richer GPU telemetry into Control evidence capture with
   configurable sample modes and CSV/current-state fields.
3. Done: add SIO voltage and temperature capture to the foreground evidence
   plane.
4. Done: extend fan evidence with tach high/low bytes and changed flags.
5. Deferred: add phase/load markers only if the analyzer summaries and decision
   records prove that manual run labels are insufficient.

Control's native CSV/JSONL/manifest logger is now the default for normal
controller validation. Bench can remain a characterization tool, but not a
required live sidecar.
