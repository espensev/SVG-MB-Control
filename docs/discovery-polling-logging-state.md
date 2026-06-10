# Discovery — Polling, Logging, and Runtime State

> Historical note, 2026-05-14: this discovery captured the state before the
> current measured `50 ms` control/write profile, fixed-start timing fields,
> process resource fields, rate-limited status publication, and per-channel
> thermal-pressure boost logging. Keep it as background context, but use
> `docs\MEASUREMENT_GATE.md` and
> `docs\RUNTIME_LOGGING_AND_EVALUATION.md` for current tuning decisions.

**Goal:** Evaluate the current `SVG-MB-Control` runtime quality and layout, with
special focus on logging, polling cadence, write cadence, and the measurement
work still needed before raising runtime rates for mixed CPU/GPU/airflow fan
control.
**Date:** 2026-04-16
**Status:** complete
**Recommended next:** none — standalone research; the next work is a direct
implementation and measurement pass, centered on cadence characterization rather
than UI or repo restructuring

---

## Questions

1. What does the current `SVG-MB-Control` runtime already own, and how mature is
   its structure?
2. What logging and runtime-state surfaces exist today, and how good are they?
3. How are polling and write cadences currently implemented, and what timing
   limitations exist?
4. What current tests validate the runtime, and what important gaps remain?
5. What measurement capability already exists elsewhere in the `SVG-MB`
   workspace for determining sensible rates?
6. What lessons from `NVG_SmoothControl` are useful here without copying its
   tray/service/UI architecture?
7. What concrete next steps best fit the current repo state?

---

## Findings

### Q1: What does the current `SVG-MB-Control` runtime already own, and how mature is its structure?

**Answer:** `SVG-MB-Control` is already a real standalone runtime repo. It owns
its executable, runtime-home contract, vendored low-level dependencies, and
packaged PawnIo resources. Structurally, it is beyond the earlier bridge-removal
phase. The repo is in a usable direct-runtime state, but its internal runtime
loop design is still conservative and not yet measurement-grade for cadence
work.

**Evidence:**
- `README.md:3` — describes `SVG-MB-Control` as the standalone runtime repo.
- `README.md:5` — states that the repo owns vendored dependencies and does not
  depend on sibling repos at runtime.
- `README.md:14-15` — states that GPU telemetry and fan reads/writes/restore
  are direct and vendored.
- `README.md:151-157` — lists Control-owned runtime files and says the JSON
  state plane remains authoritative while logging publishes active log paths.
- `config/control.release.json:7-14` — shows runtime-owned polling, rotation,
  retention, and control-loop cadence fields.

**Implications:**
- The main question is no longer repo containment.
- The main question is whether the current direct runtime is instrumented and
  structured well enough to choose better rates later.

### Q2: What logging and runtime-state surfaces exist today, and how good are they?

**Answer:** Control now has a real historical logging plane in addition to the
runtime-state JSON files. `read-loop` and `control-loop` both publish:

- live state JSON,
- a fixed-path live CSV mirror,
- rotating archive CSV chunks,
- append-only JSONL event logs,
- active log paths in `control_runtime.json`.

This is a substantial improvement over the earlier state-only phase. The main
quality issue is not missing logs; it is that the logs still lack timing-fidelity
fields needed for cadence tuning.

**Evidence:**
- `README.md:151-157` — lists `logs\svg_mb_control_output.csv`,
  `logs\svg_mb_control_events.jsonl`, and says `log_csv_path` / `event_log_path`
  are published.
- `src/runtime_logging.cpp:199-204` — resolves the fixed CSV mirror and JSONL
  event log paths.
- `src/runtime_logging.cpp:365-379` — defines the read-loop CSV header.
- `src/runtime_logging.cpp:388-399` — extends the control-loop header with
  `loop_tick_count` and per-channel control fields.
- `src/read_loop.cpp:190-205` — opens the runtime CSV logger for `read-loop`.
- `src/read_loop.cpp:249-262` — rotates when needed and appends one CSV row per
  poll.
- `src/control_loop.cpp:502-510` — opens the runtime CSV logger for
  `control-loop`.
- `src/control_loop.cpp:575-585` — logs rotation events.
- `src/control_loop.cpp:777-782` — appends one control-loop CSV row per tick.
- `src/read_loop.cpp:112-116` and `src/control_loop.cpp:450-453` — publish
  `log_csv_path` and `event_log_path` in `control_runtime.json`.

**Implications:**
- Logging is already product-owned and in-process, which is correct for this
  repo.
- The current logging subsystem is good enough to support a first real
  measurement pass.
- The next quality jump is not adding more log files; it is adding timing
  metadata and measurement discipline.

### Q3: How are polling and write cadences currently implemented, and what timing limitations exist?

**Answer:** The current Control runtime is intentionally conservative:

- `read-loop` defaults to `poll_ms = 500`.
- `control-loop` uses the characterized packaged `poll_tick_ms = 200`.
- channel write cooldowns default to `10000 ms`.

The more important limitation is architectural: both long-running loops perform
their sampling, logging, JSON publication, and event writes first, then sleep
for the configured interval. That means the configured interval is **not** the
real loop period. The real period is:

`work time + wait_for(configured interval)`

So effective cadence is lower than the nominal setting and varies with sampling
and file-I/O cost.

**Evidence:**
- `config/control.release.json:7-14` — current defaults are `poll_ms=500`,
  `poll_tick_ms=200`, and `write_cooldown_ms=10000`.
- `config/control.release.json` — the packaged live loop is limited to
  channels `0,1,2,3,4,5`; lanes `2,3` were later reintroduced for the
  higher-floor front-intake response.
- `src/read_loop.cpp:244-308` — the loop samples, writes CSV, writes JSON, and
  updates status before waiting.
- `src/read_loop.cpp:310-314` — `wait_for(poll_ms)` happens after the work.
- `src/control_loop.cpp:565-792` — the control loop samples, evaluates,
  writes sidecars, applies writes, logs CSV, and writes status before waiting.
- `src/control_loop.cpp:795-799` — `wait_for(poll_tick_ms)` happens after the
  work.
- `src/runtime_logging.cpp:330-338` — each CSV row is flushed immediately to
  both the archive file and the live mirror.
- `src/read_loop.cpp:308` and `src/control_loop.cpp:790-792` — each loop also
  rewrites `control_runtime.json` on its own cadence.
- `src/control_loop.cpp:606-607` — `control-loop` also republishes
  `current_state.json` every tick when telemetry is available.
- `src/direct_runtime_snapshot.cpp:148-162` — the runtime snapshot records
  `snapshot_time` and telemetry values, but not sample latency, loop duration,
  jitter, or effective frequency.

**Implications:**
- The current default rates are safe and intentionally bounded, but the repo is
  not yet architected to support aggressive high-rate claims.
- If the project later aims for materially faster control-loop rates, it will
  need deadline-based scheduling or explicit timing instrumentation first.
- Right now the runtime conflates several concerns on one cadence:
  - sensor sampling,
  - control evaluation,
  - hardware writes,
  - status publication,
  - CSV logging,
  - event logging.
- The 2026-05-12 update added loop timing fields and cleared the packaged
  `200 ms` control tick for lanes `0,1,2,3,4,5`. Faster rates or additional live
  channels still need fresh measurement and likely more separation.

### Q4: What current tests validate the runtime, and what important gaps remain?

**Answer:** The current smoke suite validates the direct runtime contract well
enough for correctness at the feature level, but not at the timing-performance
level. The suite passed `20/20` on this review. It proves that:

- `read-loop` and `control-loop` publish the expected files,
- active log and event paths are set,
- write-once and control-loop write flows create and clear pending-write
  sidecars,
- representative events such as `write_applied` are emitted.

What it does **not** prove:

- log rotation and retention behavior,
- CSV schema content beyond existence,
- event schema completeness,
- timing accuracy,
- effective cadence,
- jitter,
- read latency,
- write latency under load.

**Evidence:**
- Local verification on 2026-04-16: `python -m unittest discover tests -v`
  returned `Ran 20 tests in 2.832s` and `OK`.
- `tests/test_smoke.py:464-473` — asserts that `read-loop` creates status,
  event, archive, and mirror log files.
- `tests/test_smoke.py:752-758` — asserts that `control-loop` creates log files
  and records `control_loop.write_applied`.
- `tests/test_smoke.py:539-542` — asserts `write_once.write_applied` and
  `write_once.restore_applied`.
- `tests/test_smoke.py:622-649` — covers reconcile behavior.
- `tests/test_smoke.py` has no references to `log_rotated`, `rotate_hours`,
  `retain_days`, `effective_hz`, `latency`, `jitter`, or scheduler overruns.

**Implications:**
- The existing tests are good contract smoke tests.
- They are not yet the right tests for choosing faster poll or write rates.
- A separate characterization lane is required before changing defaults.

### Q5: What measurement capability already exists elsewhere in the `SVG-MB` workspace for determining sensible rates?

**Answer:** `SVG-MB-Bench` already contains most of the cadence-characterization
tooling this runtime needs, but that capability currently lives outside Control.
Bench is the existing place to measure:

- AMD sensor update cadence,
- passive SIO + AMD telemetry cadence,
- fan write response and write latency,
- scheduler jitter and effective achieved rate.

The main missing measurement lane for the mixed-input Control design is GPU
update cadence inside the MB stack.

**Evidence:**
- `SVG-MB-Bench/README.md:164-170` — `amd-rate-probe` exists to characterize
  AMD sensor update cadence.
- `SVG-MB-Bench/README.md:214-220` — `thermal-log` exists for passive AMD + fan
  capture at configurable rates.
- `SVG-MB-Bench/README.md:288-291` — `fan-write-probe` captures RPM response
  and write timing artifacts.
- `SVG-MB-Bench/README.md:398` — says effective cadence, scheduler jitter, and
  read latency are measured and recorded per row.
- `src/main.cpp:65-66` in Control — only exposes `--diagnose-amd` and
  `--diagnose-gpu` one-shot diagnostics rather than cadence probes.
- `src/gpu_reader.h:38` — Control has a GPU sample method, but no dedicated
  rate probe or cadence artifact lane.

**Implications:**
- The workspace already has a measurement culture and tooling model worth
  reusing.
- For CPU and SIO cadence decisions, the fastest path is to use Bench to gather
  the evidence.
- For GPU-assisted control decisions, the current MB workspace still lacks an
  equivalent cadence probe.

### Q6: What lessons from `NVG_SmoothControl` are useful here without copying its tray/service/UI architecture?

**Answer:** Three lessons are directly useful:

1. Keep poll cadence and write cadence as separate concepts.
2. Treat logging as a first-class runtime subsystem.
3. If steady-state suppression is ever added, do it **after** a period of
   logging every tick and only as an optional filter.

The parts that do **not** need to be copied now are shared memory, tray-driven
rotate-log commands, archive indexing, or UI surfaces.

**Evidence:**
- `NVG_SmoothControl/CONFIG_REFERENCE.md:70-71` — separates `poll_ms` from
  `write_ms`.
- `NVG_SmoothControl/README.md:85` — says both cadences are startup-latched.
- `NVG_SmoothControl/README.md:144` — publishes the active archive path as
  `logCsvPath` in status/shared memory.
- `NVG_SmoothControl/CONFIG_REFERENCE.md:77` — states that CSV logging records
  every controller tick by default.
- `NVG_SmoothControl/CONFIG_REFERENCE.md:81` — introduces an optional log
  filter only after the default full-fidelity behavior.
- `NVG_SmoothControl/src/csv_logger.h:438-498` — shows a bounded steady-state
  filter based on tolerance + liveness timeout, not blind downsampling.

**Implications:**
- Control should keep the useful design principles, not the whole NVG runtime
  topology.
- The most relevant transferable idea is **measurement first, suppression
  second**.
- The second most relevant idea is that future Control should likely separate:
  - internal sample cadence,
  - hardware write cadence,
  - file status publication cadence.

### Q7: What concrete next steps best fit the current repo state?

**Answer:** The next step should be a characterization pass, not a UI pass and
not a major refactor. The runtime is already capable enough to support this,
but the current defaults and loop design are too conservative and too
under-instrumented to justify rate changes yet.

**Evidence:**
- `config/control.release.json:7-14` — shows intentionally slow defaults.
- `src/runtime_logging.cpp:368-379` and `src/runtime_logging.cpp:388-399` —
  current CSV schemas capture values and control state, but not timing quality.
- `src/read_loop.cpp:244-314` and `src/control_loop.cpp:565-799` — current loop
  timing is fixed-delay-after-work, not measured fixed-period scheduling.
- `docs/archive/discovery-logging-parity.md:24` — still describes the repo as
  state-only and says it does not maintain append-only logs.
- `docs/archive/discovery-logging-parity.md:130-132` — still presents `runtime\logs\`
  as future work even though it is now implemented.

**Implications:**
- Some internal docs are now stale and should not be treated as current design
  truth.
- The current implementation is good enough for measurement-oriented next work.
- The next pass should focus on proving sensible rates, not on broad
  architectural reinvention.

---

## Cross-Cutting Analysis

### Constraints

- `SVG-MB-Control` must remain standalone and direct-only at runtime.
- UI work is out of scope for this pass.
- The runtime currently performs sampling, decision-making, logging, and JSON
  publication on the same main-loop cadence.
- The loop scheduler is currently fixed-delay, not fixed-deadline.
- Mixed-input control needs CPU, GPU, and airflow-relevant fan behavior, but
  only CPU and SIO cadence characterization already exist as first-class tools
  in the MB workspace.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|-----------|--------|-------|
| Raising Control poll rates before characterization | H | H | Current loops do not measure effective frequency, jitter, or latency. |
| Mistaking configured cadence for achieved cadence | H | H | `wait_for` occurs after the work, so true period is longer than configured. |
| Letting per-tick file I/O distort future high-rate tests | H | M | CSV rows flush every write, and status JSON is rewritten every loop. |
| Choosing CPU/GPU blend timing without GPU cadence evidence | H | M | GPU diagnostics exist, but no GPU rate probe exists in this MB stack. |
| Trusting `docs/archive/discovery-logging-parity.md` as current design truth | H | M | That document predates the implemented runtime logging subsystem. |

### Open Questions

- What is the actual update cadence of the current GPU telemetry path under the
  vendored `gpu_telemetry` slice?
- For this board and sensor stack, what is the highest rate at which AMD+GPU+SIO
  reads still produce materially new information?
- At what rate does per-tick JSON + CSV flushing begin to dominate loop time?
- Which fans need fast reaction for direct CPU aid, and which should stay on
  deliberately slow airflow-support cadences?
- Whether Control should eventually use one superset CSV schema for all
  long-running modes or continue with separate read-loop and control-loop row
  shapes.

---

## Recommendation

Proceed with a measurement-first next pass built around these steps:

1. Use Bench to re-characterize AMD cadence, passive AMD+SIO cadence, and
   fan-write response on this machine, because the older assumptions are likely
   stale.
2. Add a Control-side cadence probe or Bench-side GPU probe for the vendored GPU
   path so mixed-input control is not tuned blind.
3. Extend Control logging with timing-quality fields before raising rates:
   - loop start/end wall clock,
   - loop duration,
   - intended interval,
   - achieved interval,
   - overrun/slip indicator,
   - optionally read duration per sensor group.
4. Keep logging every tick during characterization.
5. Only after cadence evidence exists, decide whether Control should add:
   - a separate write cadence field,
   - a separate status-publication cadence,
   - or an optional steady-state log filter similar in spirit to NVG.

That order fits the repo as it exists now and avoids premature optimization
based on stale sensor-rate assumptions.
