# Script Stack Review

Captured: 2026-05-28

This note records the script simplification review so the items can be handled
one at a time. It focuses on scripts that duplicate native code, encode policy
that could drift, or carry more operational surface than the repo needs.

## Findings

### 1. Move analysis output into native code

Status: mostly addressed. Native `analyze report` now writes report files,
analysis manifests, and compact decision records from the SQLite ingest data.
`scripts/analyze_control_run.py` remains as a legacy direct-CSV compatibility
path for captures that have not been ingested yet.

`scripts/analyze_control_run.py` was the clearest script-to-code candidate. The
repo now has native `svg-mb-control analyze ingest`, `analyze prune`, and
`analyze report`, and the native report path owns the normal report,
decision-record, and analysis-manifest workflow for ingested runs.

Recommended direction:

- Prefer native `analyze ingest` plus `analyze report` in README and current
  evaluation docs.
- Keep `scripts/analyze_control_run.py` only for direct CSV compatibility, or
  later replace it with a thin wrapper once all required raw-CSV use cases are
  covered by native ingest/report.

Residual risk:

- Low/medium. The main operator path is native now, but the Python analyzer is
  still tested and can diverge if it keeps receiving feature work.

### 2. Reconcile or retire root build.ps1

Status: addressed. Root `build.ps1` now delegates to
`scripts/Test-LocalCI.ps1 -KeepBuildDir`.

The documented workflow says to use `build-release.ps1`,
`scripts/Build-Release.ps1`, or `scripts/Test-LocalCI.ps1`. Root `build.ps1`
does its own Visual Studio environment setup through `vcvarsall.bat`, has a
machine-specific fallback path, and directly drives CMake.

Recommended direction:

- Replace root `build.ps1` with a wrapper to `scripts/Test-LocalCI.ps1
  -KeepBuildDir`, or
- Rename/document it as a low-level debugging helper only.

Risk:

- Medium. It can lead agents or operators away from the maintained release and
  CI workflow.

### 3. Consolidate install/watchdog script helpers

Status: addressed. Shared installer helpers live in
`Install-SVG-MB-ControlCommon.ps1`; the packaged watchdog path now requires
`svg-mb-control-task-runner.exe`, and the VBS fallback was removed.

The scheduled-task, watchdog, and shortcut scripts duplicate executable
resolution and task helper logic. The native `svg-mb-control-task-runner.exe`
already handles hidden `--start`, `--restart`, `--stop`, `--status`, and
`--watchdog-run` paths.

Recommended direction:

- Require the native task runner in packaged releases.
- Drop the VBS/PowerShell hidden watchdog fallback from normal release
  packages.
- Keep PowerShell for scheduled-task registration, but consolidate shared
  helper functions or reduce the scripts to thin installers.

Risk:

- Medium. The current duplication is small enough to live with, but fallback
  paths increase drift and maintenance load.

### 4. Trim stale release-build packaging logic

Status: addressed. The unused PawnIO helper functions were removed from
`scripts/Build-Release.ps1`.

`scripts/Build-Release.ps1` defines PawnIO binary copy helpers that appear to be
unused because the release package already copies `resources`. The same script
also owns package inventory and source-archive selection directly in PowerShell.

Recommended direction:

- Remove unused `Resolve-AmdFamily17BinaryPath` and
  `Copy-DistAmdFamily17Binary`.
- Consider moving package inventory toward CMake `install()` rules or a small
  manifest once the simpler cleanup is done.

Risk:

- Low. This is mostly maintenance weight, but stale packaging paths can confuse
  future release changes.

### 5. Narrow the eval dashboard server surface

Status: addressed. The Python server now serves `tools/eval_dashboard` assets
only, with runtime access limited to explicit `/api/*` endpoints.

The dashboard server binds to localhost by default, but it serves the whole repo
root and exposes dashboard files through that root. If `-HostName` is changed,
the server exposes more files than the dashboard needs.

Recommended direction:

- Serve only `tools/eval_dashboard`.
- Keep explicit `/api/live-tail.csv`, `/api/events-tail.jsonl`, and
  `/api/health.json` endpoints for runtime data.
- Remove unused server constants while touching the file.

Risk:

- Low. Default localhost limits exposure, but narrower serving is cleaner and
  cheaper to reason about.

## Suggested Order

Completed in this pass:

1. Removed dead packaging helpers from `scripts/Build-Release.ps1`.
2. Reconciled root `build.ps1` with the documented workflow.
3. Narrowed the eval dashboard server to dashboard assets plus explicit APIs.
4. Consolidated install/watchdog scripts around the native task runner.

Remaining (resolved below): the `scripts/analyze_control_run.py`
wrapper-vs-fallback decision is settled in
[Finding 1 decision](#finding-1-decision-analyzer-wrapper-vs-fallback). A deeper
script-stack pass on 2026-05-29 recorded additional grounded, verified items in
[Deeper Pass](#deeper-pass-2026-05-29).

## Finding 1 decision: analyzer wrapper vs fallback

Decision: keep `scripts/analyze_control_run.py` as a narrowed, documented
raw-CSV fallback now; defer the thin-wrapper conversion until native `analyze`
is a superset. A wrapper today would drop four capabilities native does not yet
emit, so narrowing first is the lower-risk step.

Status: Phase A landed 2026-05-29 (see the implemented list below). Phase B
remains deferred.

Native already owns these for any run that can be ingested (so they are the
deletable, duplicated surface in the Python script):

- Report body and JSON summary: `render_markdown` /
  `summarize` (`scripts/analyze_control_run.py:538`, `:463`) vs
  `EmitTextReport` / `EmitJsonReport`
  (`src/analyze/analyze_report_emit.cpp:332`, `:226`).
- Per-channel stats: `summarize_channels`
  (`scripts/analyze_control_run.py:322`) vs `LoadChannelStats`
  (`src/analyze/analyze_report_queries.cpp:289`).
- Decision record: `build_decision_record`
  (`scripts/analyze_control_run.py:797`) vs `BuildDecisionRecord`
  (`src/analyze/analyze_report_emit.cpp:423`).
- Analysis manifest: `build_manifest`
  (`scripts/analyze_control_run.py:951`) vs `BuildAnalysisManifest`
  (`src/analyze/analyze_report_emit.cpp:97`).

Native does **not** yet cover these, so they must stay on the Python side until
ported (this is the explicit reason the script survives):

- A bare CSV (+events) with no manifest/DB. Native report requires an existing
  SQLite DB (`src/analyze/analyze_report.cpp:38`) and ingest requires a runtime
  manifest (`src/analyze/analyze_ingest.cpp:154`). The Python path reads
  `--csv`/`--events` directly (`scripts/analyze_control_run.py:1094`).
- Low-band-inclusive response-boost total: `row_response_boost`
  (`scripts/analyze_control_run.py:95`) adds
  `low_band_effective_boost_pct`/`low_band_stage_boost_pct`, which native does
  not ingest — `kTickChannelSampleColumns`
  (`src/analyze/analyze_channel_sample_columns.cpp:13`) omits the `low_band_*`
  columns the runtime CSV writes (`src/runtime/runtime_csv_rows.cpp:385`).
- GPU-envelope-peak block (peak row/elapsed time, time-above-threshold,
  first-crossing, setpoint-during-load): `summarize_gpu_response`
  (`scripts/analyze_control_run.py:381`). Native emits only band percentiles
  plus a response-delay metric.
- Loop-timing and process-resource percentiles in the report output
  (`scripts/analyze_control_run.py:489`). Native *ingests* these columns
  (`src/analyze/analyze_csv.cpp:267`) but its report does not emit them, so this
  gap is emit-side only.

Phase A — narrow now (landed 2026-05-29):

1. Deleted the native-owned functions `build_diagnostic_flags`,
   `build_decision_record`, `build_manifest`,
   `default_decision_record_path`/`resolve_decision_record_path`, and the
   now-unused `sha256_file` helper (and its `hashlib` import). Dropped the flags
   that produced those artifacts: `--manifest-out`, `--decision-record-out`,
   `--no-decision-record`, `--decision`, `--hypothesis`, `--run-id`; the
   manifest-only artifact inputs `--status`/`--current-state`/`--config`/
   `--build-info`; and the diagnostic-only `--cpu-load-threshold-c`/
   `--low-response-setpoint-pct`. The script went from 1138 to ~790 lines.
2. Converged the script's `percentile` to the native nearest-rank method
   (`math.floor((pct/100)*(n-1) + 0.5)`, matching
   `src/analyze/analyze_report_data.cpp` `std::llround`), and converged
   `tools/eval_dashboard/dashboard.js` the same way, so all three analyzers
   agree. Promoted the native diagnostic literals 35.0/10.0 to named constants
   (`kHotChannelSetpointCeilingPct`/`kSlowSetpointResponseThresholdS`) in
   `src/analyze/analyze_report_emit.cpp`.
3. Deviation from the original step-2 plan: the per-channel and event-type
   summary tables in `render_markdown` were **kept**, not dropped. Native owns
   those for *ingested* runs, but it cannot report a raw CSV at all, so for the
   raw-CSV use case the tables are this tool's value rather than duplicated
   output; the per-channel boost column also carries the low-band-inclusive
   total native does not compute. The high-severity drift hazards (manifest
   schema collision, diagnostic-threshold drift) were removed by deleting the
   functions in step 1, so keeping the read-only summary tables adds no drift
   hazard.
4. Rewrote the module docstring, `--help`, `README.md`, and
   `docs/RUNTIME_LOGGING_AND_EVALUATION.md` to the narrowed scope (percentiles
   now match native nearest-rank rather than being a divergence to caveat).
5. Rewrote `tests/test_analyzer.py` to assert only the surviving raw-CSV outputs
   (run summary, GPU-peak section, event counts), that the native-owned flags
   are gone, and that `percentile` is nearest-rank. Native paths stay gated by
   `tests/test_analyze_ingest.py` `AnalyzeReportTests`.

Phase B — convert to a wrapper later (effort L, risk medium; only if desired):
add `analyze ingest --csv` that synthesizes a run row without a manifest; add
`low_band_*` to `kTickChannelSampleColumns` plus a response-boost total; add a
GPU-envelope-peak block and a timing/resource percentile section to the native
report; extend `AnalyzeReportTests` to cover all of these; then replace the
Python body with `subprocess.run` of the in-repo `svg-mb-control.exe` (keep the
repo standalone — shell the in-repo exe, never a sibling repo).

## Deeper Pass (2026-05-29)

A second review swept the full script stack plus the largest C++ translation
units for duplication, policy drift, dead code, and split seams. Every item
below was checked against the code at the cited `file:line` and adversarially
re-verified. Items are grouped by priority. Effort is S/M/L; risk is the chance
the change breaks behavior.

Implementation status (2026-05-29): every item below landed in this pass,
including the `Build-Release.ps1` module split (Tier 4, item 11) — the 1234-line
pipeline was split into five dot-sourced, build-host-only helper files
(`Build.VsEnv.ps1`, `Build.Tools.ps1`, `Build.Package.ps1`, `Build.Info.ps1`,
`Build.Tests.ps1`), leaving `Build-Release.ps1` at 491 lines (param/config +
dot-sources + the 11-step pipeline). Tier 1's parity hazards were resolved as
part of the Finding 1 decision Phase A above (note the recorded deviation:
the analyzer's per-channel and event summary tables were kept). Code changes
were validated by `scripts/Test-LocalCI.ps1` (release build + CTest 6/6 +
hermetic Python tests, all green, exit 0); the installer scripts, which cannot
register real scheduled tasks here, were validated by PowerShell parse-checks
plus a cmdlet-stub test of the shared `Register-SvgMbControlTask` echo
order/triggers.

### Tier 1 — Analyzer parity hazards (do before any wrapper work)

These are the concrete reasons the two analyzers can disagree on the same run.
They fold into Phase A above.

1. **Manifest schema collision** (policy-drift, S, risk medium). Both analyzers
   stamp `schema = svg_mb_control.analysis_manifest.v1` on structurally
   different documents: Python is flat
   (`scripts/analyze_control_run.py:970`), native is nested
   (`src/analyze/analyze_report_emit.cpp:101`); the two shapes are pinned by
   `tests/test_analyzer.py:114` and `tests/test_analyze_ingest.py:1062`. A
   consumer keying on the schema string cannot parse both. Fix: in Phase A drop
   `build_manifest` and its dependents (the `--manifest-out` arg, the
   write/invocation block, and the decision-record manifest consumers); if a
   raw-CSV manifest must remain instead, rename the Python schema string only
   (e.g. `svg_mb_control.raw_csv_analysis_manifest.v1`) and update its test.
2. **Diagnostic-flag threshold drift** (policy-drift, M, risk medium). Two
   independent flag implementations with divergent gates and output strings:
   Python `build_diagnostic_flags` (`scripts/analyze_control_run.py:710`) uses
   configurable CPU threshold 75.0 and setpoint ceiling 35.0, a separate
   GPU branch (off by default), plus a concern-term event scan; native
   `BuildDiagnosticFlags` (`src/analyze/analyze_report_emit.cpp:189`) uses one
   `load_threshold_c` (75.0) applied to both CPU and GPU, a hardcoded 35.0
   ceiling (`:220`) and a hardcoded 10.0 s slow-response literal (`:195`). Fix:
   promote the native bare literals 35.0/10.0 to named constants in one header
   alongside the existing `kReversalDeadbandPct`
   (`src/analyze/analyze_report_queries.cpp:21`); delete Python
   `build_diagnostic_flags` in Phase A, or pin its independent 35/75/0.35
   defaults in a test if it stays.
3. **Percentile-method divergence** (duplication, S, risk low). Python and the
   dashboard JS use linear interpolation
   (`scripts/analyze_control_run.py:121`, `tools/eval_dashboard/dashboard.js:199`)
   while native uses nearest-rank (`src/analyze/analyze_report_data.cpp:12`,
   advertised at `src/analyze/analyze_report_emit.cpp:250` and `README.md:319`,
   pinned by `tests/cpp/analyze_report_tests.cpp:107`). For `{1,2,3,4,5}` p90 is
   5.0 native vs 4.6 interpolated. The Python percentile has no value-level test.
   Fix: converge the Python and JS copies to nearest-rank and add a value test
   mirroring the C++ cases, or have the Python output emit a `percentile_method`
   string so consumers know its numbers are not comparable. Note: the
   `p95`/`p99`/`avg` the Python/JS `stats()` emit have no native band-report
   counterpart, so full unification of the stat set belongs with Phase B, not
   this fix.

### Tier 2 — Cross-cutting single-source-of-truth

4. **GPU envelope rule implemented five times** (duplication, M, risk medium).
   The `max(core, memjn, hotspot-if>0)` rule lives in the control path
   (`src/control/channel_evaluator.cpp:390`, always-present inputs, type-enforced
   by `src/runtime/runtime_snapshot.h:34`), the analyzer
   (`src/analyze/analyze_csv.cpp:143`), the SQL backfill
   (`src/analyze/analyze_db.cpp:472`), Python
   (`scripts/analyze_control_run.py:253`), and JS
   (`tools/eval_dashboard/dashboard.js:249`). The analyzer/Python/JS copies are
   behaviorally equivalent (all prefer an explicit `gpu_envelope_c` column and
   are missing-value-aware); the control-path copy differs only because its
   inputs are never absent. Fix: document the optional-aware
   `analyze_csv.cpp:143` form as canonical and add a cross-reference comment on
   `GpuControlEnvelopeC`; fold the Python/JS copies in only with the analyzer
   migration. (Reframe from the earlier "tie-break" wording: the divergence is
   missing-value handling, not tie-breaking.)
5. **Response-boost suffix list duplicated four ways** (duplication, S, risk
   low). The boost component names live in `kBoostStageSpecs`
   (`src/control/boost_stage.h:58`, the CSV-header producer via
   `src/runtime/runtime_csv_rows.cpp:381`), in `kTickChannelSampleColumns`
   (`src/analyze/analyze_channel_sample_columns.cpp:20`, the ingest read path),
   in Python (`scripts/analyze_control_run.py:87`), and in JS
   (`tools/eval_dashboard/dashboard.js:312`); the `low_band_*` fallback pair is
   in none of those tables — only in `runtime_csv_rows.cpp:385`. Fix: add a
   cross-reference comment in the Python/JS lists pointing at the actual CSV
   header producer (`src/runtime/runtime_csv_rows.cpp:379`); a deeper fix is to
   have Python/JS read the CSV header dynamically (they already detect channels
   by regex).
6. **CSV line + prologue parsing in three readers** (duplication, S, risk low).
   `ParseCsvLine` (`src/analyze/analyze_csv.cpp:165`), the Python parser
   (`scripts/analyze_control_run.py:28`), and the JS parser
   (`tools/eval_dashboard/dashboard.js:69`) each re-implement RFC4180-ish
   quoting and the `# key=value` prologue convention produced by
   `src/runtime/runtime_csv_archive.cpp:277`. Fix: keep the archive writer as the
   format owner; the grammar is documented in
   `docs/RUNTIME_LOGGING_AND_EVALUATION.md:97` (not `RUNTIME_HOME.md`) — add a
   one-line cross-reference comment above each reader. Cross-runtime code sharing
   is not feasible; the goal is one documented contract plus pointers.
7. **Python interpreter resolver duplicated in two PowerShell scripts**
   (duplication, S, risk low). `Resolve-PythonRunner`
   (`scripts/Build-Release.ps1:524`, returns `@{FilePath; PrefixArgs}`) and
   `Resolve-Python` (`scripts/Start-EvalDashboard.ps1:41`, returns a path then
   re-checks for `py` at `:74`) encode the same `python`-then-`py -3` policy in
   two shapes. Fix: extract one resolver returning the `FilePath`+`PrefixArgs`
   shape into a new `scripts/`-level dot-sourced helper, source it from both, and
   add that helper to `$DistExtras` (`Build-Release.ps1:66`) since the packaged
   dashboard needs it. Do not reuse `Install-SVG-MB-ControlCommon.ps1` — it is
   installer-scoped and ships no Python logic.

### Tier 3 — Install/build policy drift

8. **Scheduled-task names hardcoded in three files** (policy-drift, S, risk
   medium). `'\SVG-MB Control\'` plus the task names live independently in
   `Install-SVG-MB-ControlScheduledTask.ps1:6`,
   `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:5`, and the combined
   `'\SVG-MB Control\SVG-MB Control Watchdog'` in `scripts/Build-Release.ps1:883`
   (used for build-time suspend/resume). If an operator installs the watchdog
   under a non-default name, `Suspend-ScheduledTaskIfEnabled`
   (`Build-Release.ps1:187`) returns `$false` with no warning and the watchdog is
   never suspended during the build. Fix: define the default task name/path once
   as constants in `Install-SVG-MB-ControlCommon.ps1` and reference them from all
   three files; emit a `Write-Host`/`Write-Verbose` note when the `schtasks`
   query returns not-found so a custom-named install is visible.
9. **Watchdog restart policy is dead in the scheduled path** (policy-drift, M,
   risk medium). `Invoke-WatchdogRun`
   (`Install-SVG-MB-ControlWatchdogScheduledTask.ps1:35`) encodes the
   exit-code→restart policy, but the registered task action runs the native
   runner `--watchdog-run` (`:154`), which has its own copy
   (`src/platform/task_runner.cpp:195`); the PowerShell body runs only on manual
   `-Run`. README documents the policy a third time (`README.md:181`). Fix: make
   the `-Run` path delegate to the native runner and delete the PowerShell
   exit-code switch (`:55`), or document `-Run` as an operator-only diagnostic
   path distinct from the scheduled action. Keep README in sync with whichever
   copy survives.
10. **Task-runner-required asymmetry** (policy-drift, S, risk low). The main
    installer resolves the runner without `-Required` and falls back to launching
    `svg-mb-control.exe` directly
    (`Install-SVG-MB-ControlScheduledTask.ps1:73`, `:89`), while the watchdog
    requires it (`Install-SVG-MB-ControlWatchdogScheduledTask.ps1:150`). In a
    real release the runner is always packaged
    (`README.md:168`, `scripts/Build-Release.ps1:63`), so the main installer's
    fallback branch is never taken. Fix: pass `-Required` in the main installer
    too and drop the fallback, or document the direct-exe fallback as an intended
    dev-tree convenience and apply it to the watchdog as well.

### Tier 4 — Smart splits (structural; defer unless the file is being touched)

11. **`scripts/Build-Release.ps1` (1242 lines, 30 functions) → dot-sourced
    modules** (smart-split, L, risk medium). One file mixes VS bootstrap
    (`:368`–`:486`), tool resolution (`:488`–`:565`), version/build-info
    (`:567`–`:638`), packaging helpers (`:223`–`:271`), archive
    (`:731`–`:840`), and test lanes (`:315`, `:842`), wired by an inline
    11-step pipeline (`:870`–`:1242`). Every helper is parameter-driven with no
    `$script:`/`$global:` shared state, and callers (`Test-LocalCI.ps1`, root
    `build-release.ps1`) shell out via `&`, so helpers can move to dot-sourced
    `.ps1` modules with no caller change. Suggested seams: VsEnv, Tools, Package,
    Info, Tests. (Place `Resolve-CTestPath` at `:295` with the Tests seam, not
    the Tools range.)
12. **Install-script consolidation** (smart-split, M–L, risk low–medium).
    Boundary A (recommended first): extract `Register-SvgMbControlTask`,
    `Get-SvgMbScheduledTask`, and `Write-SvgMbTaskInfo` into
    `Install-SVG-MB-ControlCommon.ps1` — the principal/settings block (identical
    bar `ExecutionTimeLimit`, `…ScheduledTask.ps1:95` vs
    `…WatchdogScheduledTask.ps1:163`), the `Get-*Task` wrappers (byte-identical,
    `:26` in both), and the status-info echo (`:45` vs `:102`) are duplicated.
    The helper must allow the watchdog's extra `interval_minutes:` echo line.
    Boundary B (optional): collapse the two installers into one with a
    `-Watchdog` switch; this requires updating filename references in
    `README.md`, `docs/CODE_MAP.md`, `docs/CONTROL_LOOP.md`, and
    `Build-Release.ps1` — keeping two thin wrappers over the shared helpers is
    the lower-churn default.
13. **`src/runtime/evidence_log.cpp` → `evidence_signatures.{h,cpp}`**
    (smart-split, M, risk low). Lines 25–278 are one anonymous namespace of pure
    stateless helpers (eight `*Signature` builders, three `Convert*` adapters,
    `DetectChanged`, `BuildSioEvidenceDetail`) that touch no loop/IO state;
    `RunEvidenceLog` (`:289`) is the poll-loop driver. Moving the pure block to a
    new file makes the change-detection helpers unit-testable (none are today).
    Defer unless `evidence_log.cpp` receives further feature work.
14. **`tools/eval_dashboard/server.py` `do_GET` dispatch** (smart-split, S, risk
    low). `do_GET` (`:196`) inlines redirects, tail clamping, three `/api/*`
    endpoints, and the static fallthrough; the data layer (`:25`–`:153`) is
    already free functions. Optional: replace the if-chain with an exact-path
    dispatch table plus a `_send_tail` helper for the two tail endpoints
    (`live-tail.csv`, `events-tail.jsonl`); `health.json` keeps its own method
    (it has no file/tail step). Low priority — no new module needed.

The following large C++ translation units were examined and judged **cohesive —
no split** (recorded to avoid future churn): `control_supervisor.cpp`,
`control_loop_config.cpp`, `runtime_csv_rows.cpp`, `analyze_report_emit.cpp`,
`analyze_report_queries.cpp`, `amd_reader.cpp`, `gpu_reader.cpp`,
`calibration.cpp`, `channel_evaluator.cpp`.

### Tier 5 — Dead code and micro-cleanups (trivial, safe)

15. **Stale `*.vbs` source glob** (dead-code, S, risk low). `$SourceGlobs`
    (`scripts/Build-Release.ps1:86`) still lists `'*.vbs'` after
    `Run-SVG-MB-ControlWatchdogHidden.vbs` was deleted; a repo-wide search finds
    no `.vbs` file, so the glob matches nothing. Fix: remove the `'*.vbs'` token.
16. **DistDir double-clean** (optimization, S, risk low). Step `[2/11]`
    (`scripts/Build-Release.ps1:933`) calls `New-EmptyDirectory` (remove +
    create); step `[6/11]` (`:1070`) calls `New-Item … -Force` again with no
    intervening removal, so the second call is a no-op. Fix: drop the redundant
    `New-Item`.
17. **VS version/edition guess list** (policy-drift, S, risk low).
    `Resolve-VsDevCmdPath` (`scripts/Build-Release.ps1:411`) already calls the
    `vswhere`-based `Get-VsInstallPath` first, then falls back to a hardcoded
    `@('18','2022','2019')` × edition loop (`:420`). Fix: drop the loop and let
    the function return `$null` when `vswhere` fails (the caller already throws),
    or mark the loop a last-resort path with a maintenance comment.

### Tier 6 — Documentation drift

18. **`eval_cinebench.py` description in CODE_MAP** (doc-drift, S, risk low).
    `docs/CODE_MAP.md:278` says the helper "drives the controller and extracts a
    fixed analyzer report"; the file launches nothing and computes stats inline
    from a CSV (`tools/eval_cinebench.py:91`). Fix: reword to an offline/ad-hoc
    CSV analyzer that summarizes a Cinebench-load window; remove both phrases.
19. **`eval_cinebench.py` hardcoded absolute paths** (policy-drift, S, risk low).
    `CSV_PATH`/`EVENTS_PATH` (`tools/eval_cinebench.py:12`) pin one operator's
    machine layout and one dated capture, with no argparse; on any other checkout
    it raises `FileNotFoundError` at `:93`. It also re-implements
    `data_lines`/`maybe_float`/`percentile`/`stats` that exist in
    `scripts/analyze_control_run.py` (with a different empty-input convention and
    no NaN/inf guard). Fix: add `--csv`/`--events` argparse (matching the
    analyzer's run-path interface), or relocate it out of `tools/` if it is a
    one-off scratch script; decide alongside the analyzer wrapper question.
20. **`build.ps1` in a historical discovery doc** (doc-drift, S, risk low).
    `docs/discovery-next-logging-targets.md:153` still calls root `build.ps1`
    "untracked"; it is now tracked and delegates to
    `scripts/Test-LocalCI.ps1 -KeepBuildDir` (`docs/CODE_MAP.md:251`). Per
    `AGENTS.md:35` the `discovery-*.md` set is historical, so no change is
    required; optionally add a dated note rather than silently editing it.
