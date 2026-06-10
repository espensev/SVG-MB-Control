# Discovery - Next Logging Targets

**Goal:** Move to the next logging target while accounting for work already present in the current tree.
**Date:** 2026-05-16
**Status:** complete - evidence logger includes SIO voltage/temperature, fan tach hi/lo fields, configurable GPU sample modes, richer GPU row fields, per-backend read timing, poll cadence, change flags, analyzer-generated compact decision records, and normalized event severity/error codes; focused evidence/analyzer tests and full no-publish local CI passed
**Recommended next:** Refactor `RunUntilStopped()` only after current response tuning stabilizes and analyzer-backed behavior baselines exist.

> Historical note (2026-06-09): the `50 ms control path` references below describe the
> prior shipped cadence. The current shipped profile is `poll_tick_ms = 250` /
> `write_cooldown_ms = 250` (see `config/control.release.json`).

---

## Questions

1. What logging/refactor work is already present in the current tree?
2. What current entry points can host a read-only evidence logger without touching the control loop?
3. Which existing capture primitives already expose Bench-grade evidence?
4. What tests or docs already cover the new logging/artifact layer?
5. What are the next targets, ordered by value and risk?

---

## Findings

### Q1: What work is already present?

**Answer:** The tree now has more than the artifact split. It also contains hot-path control-loop work: throttled `current_state.json` writes, a cached pending-writes sidecar, and shared timestamp helper cleanup.

**Evidence:**
- `src/runtime_artifacts.cpp:90` - latest CSV path is centralized in the new runtime artifact layer.
- `src/runtime_artifacts.cpp:100` - latest manifest path is centralized in the new runtime artifact layer.
- `src/runtime_artifacts.cpp:201` - runtime manifests now expose `session_stop`.
- `src/runtime_artifacts.cpp:206` - runtime manifests now expose `rows_written`.
- `src/runtime_artifacts.cpp:208` - runtime manifests now expose `events_written`.
- `src/control_loop.cpp:459` - control-loop snapshot publication is throttled with a 1000 ms minimum interval.
- `src/control_loop.cpp:465` - control-loop now uses `PendingWritesStore`.
- `src/pending_writes.h:47` - pending-write removals are intentionally queued until `Flush()`.
- `src/pending_writes.cpp:121` - pending-write upserts persist synchronously to preserve crash recovery.
- `src/control_scheduler.cpp:20` - the local ISO timestamp helper has been moved into shared scheduler code.

**Implications:**
- Do not start the evidence logger implementation as if the tree only contains the logger split.
- The hot-path changes deserve stabilization before adding another long-running logging surface.
- The timestamp helper probably wants a neutral home later; `control_scheduler` is an odd dependency for read-loop, lifecycle, and snapshot code.

### Q2: What entry point should host the read-only evidence logger?

**Answer:** The tree now has a foreground-only `--mode evidence-log` entry point. Existing long-running supervision still only accepts `read-loop` and `control-loop`, so evidence capture is deliberately not part of the detached service path yet.

**Evidence:**
- `src/main.cpp:53` - `RunMode` now includes `kEvidenceLog`.
- `src/main.cpp:207` - only read-loop and control-loop are treated as supervised long-running modes.
- `src/main.cpp:990` - wide-string `--mode` parsing accepts `evidence-log`.
- `src/main.cpp:1012` - config `default_mode` parsing accepts `evidence-log`.
- `src/main.cpp:1261` - supervisor launch still rejects anything outside read-loop and control-loop.
- `src/main.cpp:1449` - evidence-log runs as a foreground mode with a control config.
- `src/evidence_log.cpp:49` - evidence artifacts use separate latest CSV/event/manifest names.

**Implications:**
- The lowest-risk product shape is now in place: `--mode evidence-log` runs foreground-only.
- Detached evidence capture still needs an explicit lifecycle design if it should run beside the controller.
- Foreground evidence mode proves artifact names and log finalization without changing supervisor behavior.

### Q3: Which capture primitives already expose richer evidence?

**Answer:** The vendored backends already expose most of the evidence; Control's current wrappers intentionally narrow it for control-loop use.

**Evidence:**
- `src/gpu_reader.cpp:52` - Control samples GPU telemetry with `GpuSampleMode::ThermalFast`.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_sensor_reader.h:12` - the GPU reader supports multiple sample modes.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_sensor_reader.h:18` - `GpuSampleMode::Full` exists.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:73` - GPU snapshot includes utilization fields.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:84` - GPU snapshot includes VRAM fields.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:96` - GPU snapshot includes NVML power.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:110` - GPU snapshot includes core voltage.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:113` - GPU snapshot includes PCIe throughput/link fields.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h:119` - GPU snapshot includes throttle reasons.
- `third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:42` - public SIO API exposes voltage snapshots.
- `third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:49` - public SIO API exposes SIO temperature snapshots.
- `third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:130` - public SIO API exposes `read_voltages`.
- `third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:133` - public SIO API exposes `read_sio_temperatures`.
- `third_party/SVG-MB-SIO/src/fan_sio.h:56` - internal fan state has `tach_hi_raw`.
- `third_party/SVG-MB-SIO/src/fan_sio.h:57` - internal fan state has `tach_lo_raw`.
- `src/fan_writer.h:30` - Control's current `FanChannelState` only carries combined `tach_raw` plus duty/mode raw fields.

**Implications:**
- Evidence capture now uses evidence-specific structs and wrapper methods instead of overloading the control-loop `RuntimeSnapshot`.
- Fan high/low raw tach bytes are captured through Control's SIO wrapper without widening the vendored public fan state.
- GPU full sampling stays out of the 50 ms control path and is configurable for the evidence loop.

### Q4: What coverage exists?

**Answer:** Manifest compatibility fields now have direct read-loop coverage. Pending-write cleanup has a focused policy-refusal regression test. Evidence-log has a focused artifact split test. One earlier full no-publish CI run failed due a transient staged-exe cleanup lock; rerunning that exact smoke test passed, and subsequent full no-publish CI runs passed, including the evidence-log addition.

**Evidence:**
- `tests/test_read_loop.py:120` - read-loop shutdown test checks finalized runtime manifest behavior.
- `tests/test_read_loop.py:162` - test asserts `session_stop`.
- `tests/test_read_loop.py:163` - test asserts `rows_written == row_count`.
- `tests/test_read_loop.py:173` - test asserts `events_written`.
- `tests/test_control_loop.py:13` - control-loop tests exercise ticks, writes, CSV, events, and status.
- `tests/test_control_loop.py:398` - restore-timeout test exercises sidecar preservation on failure.
- `tests/test_control_loop.py` - policy-refusal coverage verifies pending-write removal is flushed after a failed/refused write is cleared.
- `tests/test_evidence_log.py:13` - evidence-log coverage verifies separate CSV/events/manifest artifacts and no controller latest-file collision.
- `tests/test_smoke.py:217` - restart smoke test covers stop/relaunch of a staged background worker.
- Command result, 2026-05-16: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` built successfully but failed on `test_restart_stops_and_relaunches_background_worker` because a temp staged exe was still locked during cleanup.
- Command result, 2026-05-16: rerunning `python -m unittest tests.test_smoke.SmokeTests.test_restart_stops_and_relaunches_background_worker -v` passed.
- Command result, 2026-05-16: rerunning `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed all 46 tests with publishing skipped.
- Command result, 2026-05-16: after adding evidence-log, `python -m unittest tests.test_evidence_log -v` passed.
- Command result, 2026-05-16: after adding evidence-log, `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed all 48 tests with publishing skipped.
- Command result, 2026-05-16: after adding configurable GPU evidence fields, `python -m unittest tests.test_evidence_log -v` passed.
- Command result, 2026-05-16: after adding configurable GPU evidence fields, `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed all 48 tests with publishing skipped.

**Implications:**
- Treat the current tree as buildable and test-clean, with a watched smoke-test cleanup flake.
- Add focused tests for `PendingWritesStore` flush semantics before extending the loop further.
- Consider making staged-launch smoke cleanup wait for both supervisor and worker exit before the temp directory is removed.

### Q5: What should go next?

**Answer:** The next targets should stay staged. The current dirty tree is now covered by an additional pending-write cleanup test, the runtime artifact layer has been parameterized, and the foreground evidence logger now carries richer SIO and GPU hardware evidence without touching the controller hot path.

**Evidence:**
- `src/runtime_artifacts.h:13` - `RuntimeArtifactNaming` defines archive prefix and latest CSV/event/manifest names.
- `src/runtime_artifacts.cpp:95` - latest CSV path can now be resolved from caller-supplied artifact naming.
- `src/runtime_artifacts.cpp:106` - event log path can now be resolved from caller-supplied artifact naming.
- `src/runtime_artifacts.cpp:117` - latest manifest path can now be resolved from caller-supplied artifact naming.
- `src/runtime_artifacts.cpp:123` - `RuntimeCsvLogger` accepts `RuntimeArtifactNaming`.
- `src/runtime_artifacts.cpp:211` - manifest event counts now follow the logger's artifact naming.
- `src/runtime_artifacts.cpp:403` - `AppendRuntimeEvent` accepts artifact naming.
- `src/evidence_log.cpp:49` - evidence latest files are `svg_mb_control_evidence.csv`, `svg_mb_control_evidence_events.jsonl`, and `svg_mb_control_evidence_manifest.json`.
- `src/evidence_log.cpp:74` - evidence-log opens `RuntimeCsvLogger` with the evidence naming set.
- `src/gpu_reader.h` - `GpuEvidenceSample` carries clocks, utilization, VRAM, fan, power, PCIe, throttle, and raw thermal-slot fields.
- `src/gpu_reader.cpp` - `SampleEvidence()` accepts `thermal-fast`, `fast`, `medium`, `slow`, `rare`, or `full` while `Sample()` remains thermal-fast.
- `src/runtime_logging.cpp` - evidence CSV rows append fixed GPU evidence columns after the SIO evidence columns.
- `src/control_config.cpp` - `evidence_gpu_sample_mode` is parsed from control config and defaults to `full`.
- `tests/test_evidence_log.py:67` - regression coverage asserts evidence-log does not publish controller-owned `current_state.json`.
- `tests/test_evidence_log.py:75` - regression coverage asserts evidence-log does not overwrite the default controller latest CSV.

**Implications:**
- A second logger now uses `RuntimeCsvLogger` without overwriting the controller's latest CSV/manifest/event surfaces.
- SIO and GPU evidence-schema widening are now implemented.
- The evidence logger now has per-backend read duration/cadence and optional
  change flags, so stale or unchanged backend samples can be separated from
  low controller response.
- Evidence artifacts now have their own latest files while archive files remain durable.

---

## Cross-Cutting Analysis

### Constraints

- Keep Control standalone; do not add a runtime dependency on the sibling Bench repo.
- Keep the 50 ms control path narrow. Full GPU/SIO evidence belongs outside `control-loop`.
- Do not make another logger write the existing controller latest files.
- Use the repo build workflow (`build-release.ps1` / `scripts/Build-Release.ps1`) for validation; the untracked root `build.ps1` should not become the default workflow without reconciling the repo instructions. (Update 2026-05-29: `build.ps1` is now tracked and reconciled — it delegates to `scripts/Test-LocalCI.ps1 -KeepBuildDir`; see `docs/CODE_MAP.md`.)
- Existing uncommitted changes in hot-path files should be preserved and verified rather than overwritten.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Evidence logger collides with controller latest artifacts | High | High | Current artifact paths are fixed to one latest CSV, event log, and manifest. |
| Adding full GPU/SIO reads to control-loop adds latency | Medium | High | Existing `GpuReader` uses `ThermalFast`; full evidence is measurement-grade. |
| Pending-write cache regression breaks recovery | Medium | High | Upsert is synchronous, but remove batching needs focused tests. |
| Staged process cleanup stays flaky | Medium | Medium | One local CI run failed from a locked temp staged executable; direct rerun passed. |
| Timestamp helper lands in the wrong abstraction | Medium | Low | `control_scheduler` is currently used as a shared utility source. |

### Open Questions

- Should the evidence logger be `--mode evidence-log`, `analyze evidence-log`, or a foreground-only `evidence-log` subcommand first?
- Should evidence artifacts reuse the current manifest schema with a distinct mode, or use `svg_mb_control.evidence_log_manifest.v1`?
- Should `evidence_gpu_sample_mode` stay defaulted to `full`, or should shipped configs use `medium` for lower overhead during long evidence runs?

---

## Recommendation

Ready for a small implementation sequence.

1. Done: stabilize current dirty work with no-publish CI and focused pending-write flush coverage.
2. Done: parameterize runtime artifact naming so multiple log producers can coexist under one runtime home without clobbering latest CSV/manifest/event files.
3. Done: add a minimal foreground evidence logger that reuses the artifact layer and starts with current runtime snapshot fields only.
4. Done: add SIO voltage/temperature and fan tach high/low evidence.
5. Done: add configurable GPU sample modes and richer GPU row fields.
6. Done: add per-backend read timing/cadence fields and optional change flags
   to the foreground evidence logger.
7. Done: add analyzer-generated compact decision records for response-tuning
   runs.
8. Done: add normalized event severity/error codes for response-tuning runs.
9. Done: add an explicit circuit-breaker reset command/operator workflow for
   live recovery without process restart.
10. Next: refactor `RunUntilStopped()` only after current response tuning
   stabilizes and analyzer-backed behavior baselines exist.
