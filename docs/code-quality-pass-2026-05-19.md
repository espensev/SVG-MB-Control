# Code Quality / Optimization Pass — 2026-05-19

Read-only assessment of `src/` (64 files, ~16.5k lines). No code was modified.
Scope requested: hot-path performance, organization / file responsibility,
duplication / code quality. Findings below are ranked by payoff (impact ÷
risk). Every item is anchored to `file:line` and verified against the source.

Project constraints respected throughout: control-loop steady-state sampling
must stay in-process (no separate executable / bridge / subprocess adapter);
`one-shot` and `read-loop` stay direct in-process. No proposed change violates
these.

## Codebase health summary

The codebase is in good shape structurally. Notable positives, verified:

- **Zero** `TODO`/`FIXME`/`HACK`/`XXX` markers anywhere in `src/`.
- No `#if 0` blocks, no commented-out code, no dead functions found by
  inspection.
- No empty `catch` blocks; ~70 catch sites all log, set state, or return a
  typed fallback.
- The `analyze/` subsystem, sensor readers, and fan-writer split are cleanly
  factored — no duplication between them.
- Status writing is already correctly split out of the hot loop
  (`control_status_writer.cpp`), and `control_loop_config.cpp` is already
  decoupled from `control_loop.cpp` — good existing seams.

The findings are therefore targeted, not a rewrite. Two of them are latent
**correctness** divergences hiding inside copy-pasted helpers, which is why
deduplication ranks high here — it removes bugs, not just lines.

---

## Tier 1 — Correctness divergences in duplicated helpers (do these first)

### 1. `JsonUInt32Or` accepts integer `0` in one copy, rejects it in two others — S
Three byte-identical copies except for one comparison operator:

- `src/runtime_supervisor_state.cpp:32` → `if (raw >= 0 && raw <= UINT32_MAX)`
- `src/control_supervisor.cpp:195` → `if (raw > 0 && raw <= UINT32_MAX)`
- `src/runtime_health.cpp:41` → same `> 0` form

**Problem:** For a JSON value that is the signed integer `0`,
`runtime_supervisor_state` returns `0`; `control_supervisor` and
`runtime_health` fall through to the fallback. Same key, same file format, two
different answers depending on which reader parses it. This is a behavioral
bug, not a style nit. (Note: a `0` stored as JSON *unsigned* hits the
`is_number_unsigned()` branch in all three and returns `0` — the divergence
only bites when the value is serialized as a signed integer, which is
format-dependent and fragile.)

**Fix:** Move `JsonStringOr` / `JsonUInt32Or` / `JsonBoolOr` into
`json_io.{h,cpp}` (all three TUs already `#include "json_io.h"`), delete the
local copies, pick one semantic. Recommend `>= 0` (a configured/stored value
of `0` should round-trip, not silently become the fallback). Removes ~55–60
lines and eliminates the divergence. Sites: `control_supervisor.cpp:176,186`,
`runtime_health.cpp:31,41,57`, `runtime_supervisor_state.cpp:13,23`.

### 2. `ParseLocalIso8601` sets `tm_isdst` in one copy, not the other — S/M
- `src/runtime_logging.cpp:17` — sets `tm.tm_isdst = -1` (let libc resolve DST)
- `src/runtime_health.cpp:66` — does **not** set `tm_isdst`

**Problem:** An uninitialized/zero `tm_isdst` forces a non-DST interpretation;
`-1` lets the runtime decide. The two parsers produce timestamps that differ by
the DST offset for the same input string during daylight-saving periods. The
health-display path and the CSV-row path disagree on what a logged local time
means.

**Fix:** Single shared `ParseLocalIso8601(string_view)` with `tm_isdst = -1`,
in a shared util TU. ~18 lines removed plus the divergence. Risk S/M —
timestamp semantics; run the Python test suite after.

---

## Tier 2 — Hot-path performance (per-tick at 4 Hz, `poll_tick_ms=250`)

Frequency ground-truth established by tracing `ControlLoop::RunUntilStopped`
(`control_loop.cpp:412`, loop body ~:819). Items correctly already throttled
are listed at the end so they are not re-litigated.

> The true wall-time dominator per tick is `fan_writer->ReadAllChannels()` +
> PawnIO CCD reads inside `SampleDirectRuntimeSnapshot`
> (`direct_runtime_snapshot.cpp:171-173`). This is **intrinsic to direct
> sampling and out of scope** to relocate per project rules. It is the reason
> the allocation-level wins below are second-order — worth doing, but they do
> not move the dominant cost.

### 3. CSV row written to two streams every tick + stacked `ostringstream` chain — M
- `src/runtime_artifacts.cpp:443-444` — `RuntimeCsvLogger::WriteRow` writes the
  identical `row` to `archive_stream_` **and** `mirror_stream_` every call (2×
  disk writes per tick for one logical row).
- `src/runtime_logging.cpp:429-522` — `BuildControlLoopCsvRow` builds a
  `std::ostringstream`, which calls `BuildCommonCsvPrefix` (another
  `ostringstream`, `:140`) and `BuildAmdSensorSummary` (a third, `:92`), each
  ending in a heap-allocating `.str()`.
- `src/control_status_writer.cpp:107` — `BuildChannelLogStates` allocates a
  fresh `std::vector<RuntimeControlChannelLogState>` with per-channel
  `std::string` copies every tick (called from `control_loop.cpp:1014`).

**Problem:** ~3 `ostringstream` allocations + multiple string copies + a vector
allocation + 2 file writes, every 250 ms.

**Fix:** (a) Make the "latest"/mirror file a periodic copy of the archive (or
gate mirror writes to the flush interval) instead of a parallel per-row
stream. (b) Replace the `ostringstream` chain with a reused `std::string`
member buffer (`reserve` once, `clear()` per tick) + `std::to_chars`; pass
channel log state through a reusable buffer rather than returning a new vector
each tick. Covered by `tests/test_smoke.py` / `tests/test_read_loop.py`.

### 4. `RuntimeSnapshot` + reader snapshots constructed and returned by value every tick — M
`src/direct_runtime_snapshot.cpp:154-175` returns a fresh `RuntimeSnapshot`
each tick containing `std::vector<RuntimeAmdSensor>` and
`std::vector<RuntimeFanSnapshot>` (each element holding a `std::string label`)
plus GPU strings. `MergeAmdTelemetry` (`:101-106`) does `clear()` + `reserve()`
+ `push_back` into a new vector; `amd_reader.Sample()` returns its own
`std::vector<AmdTemperatureSample>` by value. Even with NRVO on the outer
object, the inner vectors + string elements are allocated and freed every tick.

**Fix:** Hold `RuntimeSnapshot` and the reader snapshots as reusable members of
the loop `Impl`; `clear()` vectors in place and overwrite fields. Optionally
let `AmdReader::Sample` fill a caller-provided snapshot. Touches reader
signatures; well covered by tests.

### 5. AMD CCD label string allocated per CCD per tick — S
`src/amd_reader.cpp:654` builds `"CCD" + std::to_string(index + 1u) + " (Tdie)"`
for every valid CCD every `Sample()` — up to 8 string allocations/tick (~32/s),
then copied again into `RuntimeAmdSensor.label`. The labels are constant.

**Fix:** Static precomputed label table (`"CCD1 (Tdie)"`…`"CCD8 (Tdie)"`),
index into it. Trivial, near-zero risk.

### 6. `SampleProcessResources()` called every tick, consumed only on ≥1000 ms windows — S
`src/control_loop.cpp:963` calls `SampleProcessResources()` (two Win32 syscalls
— `GetProcessTimes`, `GetProcessMemoryInfo`, `control_scheduler.cpp:70-97`)
every tick, but `UpdateTimingResources` only acts when
`resource_window_ms >= 1000.0` (`:976`). ~75% of ticks take the sample and
discard it.

**Fix:** Sample only when the ≥1000 ms window is about to close (or when a
status/CSV write is due); cache working-set/private bytes between samples.

### 7. `WaitForNextControlTick` uses a mutex + condition_variable that is never notified — S
`src/control_scheduler.cpp:146` waits on `context.wake_cv`
(`control_runtime_context.h:93`). Verified: this CV is **never** notified
anywhere in `src/` — the only notified CV is `read_loop.cpp`'s separate,
identically-named one (`read_loop.cpp:111`). So this is a timed sleep wrapped
in an uncontended mutex + predicate re-check, and **stop latency is up to one
full `poll_tick_ms` (250 ms)** because nothing wakes it early on stop.

**Fix:** Either replace with `std::this_thread::sleep_until(deadline)` plus a
short `stop_flag` poll for prompt shutdown, or actually `notify_one()` the CV
from the stop path so shutdown is immediate. Verify against stop-handling
tests — this changes shutdown latency (an improvement).

**Status 2026-05-19 (re-verified, resolved):** The "never notified" premise
is outdated. `ControlLoop::RequestStop()` (`control_loop.cpp:605-607`)
`notify_all()`s `wake_cv` under `wake_mutex`, and the console handler sets
`g_stop_signaled` — the `stop_flag` passed to `RunUntilStopped`
(`main.cpp:527`) — before calling it, so the interactive/console stop path is
already immediate. The residual was the supervisor `--stop` path:
`WaitForNextControlTick` did not check `RuntimeStopRequested(runtime_home)`
and a cross-process `stop.request.json` cannot notify `wake_cv`, so it was
observed up to one `poll_tick_ms` late. Resolved by a bounded-slice wait —
predicate also checks `RuntimeStopRequested`, re-checked every 50 ms — in
`control_scheduler.cpp`, mirroring the existing `read_loop.cpp:349-356`
pattern.

### Already correctly throttled — no action
- `WriteLowBandEvidenceFile` (`control_loop.cpp:248`, called `:1025`): every
  ~5000 ms (`evidence_write_interval_ms`), **not** per tick. Verified.
- Runtime snapshot file write (`:886`): throttled to 1000 ms.
- Status file write (`:1046`): every 10 ticks (2.5 s).
- `pending_store.Flush()` (`:931`): early-returns when not dirty
  (`pending_writes.cpp:159-164`) — no steady-state cost.
- `AppendRuntimeEvent` and the authority-reassert / stage-activation
  `ostringstream`s: transition-only cold paths.

---

## Tier 3 — Pure deduplication (lines removed, no behavior change)

### 8. `GetEnvOrDefault` — 5 character-identical copies — S
`fan_writer.cpp:18`, `simulated_fan_writer.cpp:18`, `gpu_reader.cpp:18`,
`amd_reader.cpp:49`, `direct_runtime_snapshot.cpp:19`. Identical `_dupenv_s`
wrappers. Extract into one `env_util.{h,cpp}` (or an existing util TU), delete
5 copies. ~70 lines, zero behavior change.

### 9. `IsProcessActive` — 2 identical copies — S
`control_supervisor.cpp:202` and `runtime_health.cpp:86`, byte-identical
`OpenProcess`/`GetExitCodeProcess`/`CloseHandle`. Fold into the same shared
runtime-util TU as item 2. ~13 lines.

### 10. Ignored `TryWriteJsonFileAtomic` return at 4 sites — S
`control_loop.cpp:320` (`low_band_evidence.json`), `runtime_health.cpp:181`
(`control_health.json`), `runtime_artifacts.cpp:356` and `:357` (active
manifest + manifest). The other 8 callers check the `bool`. A silent
manifest/health write failure is currently invisible. At minimum, log on
`false` at these 4 sites to match the checked callers.

---

## Tier 4 — Organization / file responsibility

Size alone was not treated as a defect. `amd_reader.cpp` (671) and
`control_loop_config.cpp` (537) were checked and are cohesive single-purpose
units — left alone. The items below are genuine mixed-responsibility or
misleading-name issues.

### 11. Extract analyze CLI dispatch out of `main.cpp` — S, highest org payoff
`main.cpp:113-366` (`PrintAnalyzeUsage`, `ResolveAnalyzeRuntimeHome`,
`ParseUint32Option`, `RunAnalyzeCommand`, ~250 lines) is pure CLI
parsing/dispatch for the `analyze` verb; everything it calls is already
namespace-isolated in `svg_mb_control::analyze` with its own `analyze/`
directory + CMake group. `wmain` only forwards (`main.cpp:615-617`). Move to
`analyze/analyze_cli.cpp` exposing `int RunAnalyzeCommand(int, wchar_t**)`.
`main.cpp` shrinks ~24%; the offline analyze tool becomes fully
self-contained. Cleanest cut in the codebase.

### 12. Rename `runtime_logging.{h,cpp}` → `runtime_csv_rows.{h,cpp}` — S
`runtime_logging.cpp` (524 lines) contains *only* CSV header/row composition
(`BuildReadLoopCsvRow`, `BuildEvidenceLogCsvRow`, `BuildControlLoopCsvRow`,
`BuildCommonCsvPrefix`). Actual event logging is elsewhere
(`runtime_artifacts.cpp::AppendRuntimeEvent`). The "logging" name actively
misleads and collides conceptually with the event log in `runtime_artifacts`.
Pure mechanical rename + include fixups, no logic change.

### 13. Extract low-band evidence out of the hot-loop TU — M
`control_loop.cpp:248-322` (`WriteLowBandEvidenceFile`, pure JSON
serialization of `ControlRuntimeContext`) plus the evidence aggregation in
`UpdateLowBandState` (`:151-245`: rpm sums, boost-area integration, sample
counts) is evidence bookkeeping, not control decision-making. It is a
structural sibling of the already-separate `gpu_evidence_csv.cpp`. Extract to
`low_band_evidence.cpp` taking `const ControlRuntimeContext&` /
`ControlRuntimeContext&`. **Stays fully in-process** — no constraint
violation. Risk M: state is threaded through `ControlRuntimeContext`, so the
seam is the context struct, not a clean function boundary.

### 14. `runtime_artifacts.cpp` holds three responsibilities — M
Mixes (a) path-resolution free functions (`ResolveRuntimeLogsDir/ArchiveDir/…`,
~:123-165), (b) `RuntimeCsvLogger` class with rotation/manifest/prune
(~:166-490), and (c) `AppendRuntimeEvent` + event-count cache (`:56-95`,
`:491+`) behind a vague "artifacts" name. Split into `runtime_paths`,
`runtime_csv_archive`, `runtime_event_log`. Risk M — `AppendRuntimeEvent` has
wide call-site fanout (control_loop, read_loop, evidence_log, supervisor);
mechanical but broad. Resolves the naming ambiguity vs. item 12.

### Lower-value org items (S each, listed for completeness)
- `main.cpp:390-498` startup-banner printers — presentation-only, belong with
  their loops or a `startup_banner.cpp`.
- `main.cpp:543-595` `ParseCalibrationSequence` — calibration token grammar;
  belongs in `calibration.cpp`, leaving only generic parsers in main.
- `control_supervisor.cpp:437-510` `PrintRuntimeStatus` — read-only status
  *display*, not supervision; belongs in `runtime_health.cpp`.

---

## Recommended order if acting

1. **Items 1 & 2** — correctness divergences. Highest value, smallest risk.
2. **Items 8, 9, 10** — pure dedup + the missing write-failure logging; land
   alongside 1 & 2 in one "shared helpers" change (they share the new util TU).
3. **Items 5, 6, 7** — small hot-path wins, near-zero risk.
4. **Item 11** — extract analyze CLI; isolated, high org clarity.
5. **Items 3, 4** — the real per-tick allocation/I/O wins (M risk, test-covered).
6. **Items 12, 13, 14** — larger reorganizations; do after the above settle.

Note none of these moves the dominant per-tick cost (hardware sampling, which
is constrained to stay in-process). They reduce allocation churn, remove two
latent bugs, and improve shutdown latency and file organization.

---

*Generated by a read-only analysis pass. Pick which items to act on; no edits
were staged.*
