# FEAT-0020 implementation plan — standard control-loop power logging (CPU + GPU)

**Project:** svg-mb-control
**Status:** Implemented + live flip executed 2026-06-18 (gate 6 closed; PR [#20](https://github.com/espensev/SVG-MB-Control/pull/20))
**Updated:** 2026-06-18
**Companion spec:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`
**Decision record:** `docs/power-logging-flip-plan-2026-06-18.md` (D-PWRLOG-1, `Current`)

**Archive status:** archived 2026-06-20 as an implemented plan. Use it for audit
history only; current verification lives in the companion spec,
`docs/TRACEABILITY.md`, and `docs/feat-0020-live-flip-validation-results-2026-06-18.md`.

This document evaluates the existing flip plan / FEAT-0020 spec against the real
source and gives a grounded implementation plan. Every claim below was verified
against source with a `file:line` citation by a 7-dimension verification pass; the
three highest-stakes corrections (C1 analyzer positional schema, C2 instantaneous
GPU mW, C6 active safety-revert) were independently spot-checked against
`analyze_ingest_db.cpp:156-175`, `gpu_snapshot.h:95-102`, and
`Reset-EnergyToDisabled.ps1:56-75`. **This is a plan; no product code is written
by it, and it does not by itself authorize a release publish or live flip.**

---

## Implementation status (updated 2026-06-18, post-build evaluation)

Tracks B/C/D/E/F are implemented in the working tree and validated by
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir` on 2026-06-18:
Release build passed, CTest `13/13`, and hermetic Python tests `169/169`.
Landed:

- **Track B (GPU side):** `GpuTempSample` power fields (`gpu_reader.h`), a
  board-power-only NVML read helper on the per-tick thermal-fast path
  (`gpu_probe.cpp` + `gpu_reader.cpp`), read identity (`gpu_power_sample_id` /
  `gpu_power_time_ms`), `MergeGpuTelemetry` plumb
  (`direct_runtime_snapshot.cpp`), `RuntimeGpuSnapshot` fields
  (`runtime_snapshot.h`), and 5 control-loop CSV columns
  (`runtime_csv_rows.cpp`).
- **Track C (analyzer):** schema `v10→v11` (`analyze_db.{h,cpp}` + migration
  ladder), CSV parse (`analyze_csv.{h,cpp}`), positional INSERT `?42..?46`
  (`analyze_ingest_db.cpp`), `SummariseGpuPower`/`ComputeGpuPower`
  mean+p50/p90/max (`analyze_report_queries.cpp`, `analyze_report_data.{h,cpp}`),
  JSON+text emit (`analyze_report_emit.cpp`).
- **Track D (operator workflow):** `scripts/Set-EnergyLoggingProfile.ps1`
  `-Enable/-Disable/-DryRun`, packaged via `scripts/Build-Release.ps1`.
- **Track E (operator/runtime docs):** `README.md`, `docs/RUNTIME_HOME.md`, and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` describe the new fields and flip
  workflow.
- **Track F (tests):** CSV header/row (`csv_rows_tests.cpp`,
  `test_control_loop.py` incl. no-false-zero), analyzer migration v9→v11 +
  old-archive degrade + GPU distribution-vs-integral (`test_analyze_ingest.py`),
  and operator dry-run workflow (`test_energy_logging_profile.py`).

### Hot-path correction closed

The initial implementation routed `sample_thermal_fast` through the full
`poll_power` helper, which can also issue the undocumented NVAPI per-rail
topology read. That was corrected before commit: `gpu_probe.cpp` now splits
`poll_nvml_board_power` from the richer `poll_power` path, and the per-tick
thermal-fast path calls only the NVML board-power helper. The slower evidence
tiers still use full `poll_power` where `power_rails` are legitimate evidence.

### Remaining work (not yet done)

1. Release publish remains a separate authorization gate.
2. Live flip/restart remains a separate live-runtime authorization gate.
3. Live M-evidence gate (gate 6 / REQ-PWRLOG-04) — separately authorized; capture
   a same-machine/same-build **pre-flip 250 ms baseline** as the comparator first.

### Build/packaging obligations (folded from the retired build-chain review)

The existing chain is adequate; **no new build entrypoint is needed.** Use
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (= `Build-Release.ps1
-NoStopProcesses -NoPublish`: build, package to `dist\`, CTest, pytest discovery;
no `release\` publish, no live-worker stop). The current FEAT-0020 edit areas are
already in `svg_mb_control_core`, and the C++ tests extend existing registered
test executables, so **no CMake change is required by the work landed so far.**
Obligations that trigger only when the corresponding file is added:

- When `scripts\Set-EnergyLoggingProfile.ps1` is created, add it to `$DistExtras`
  in `scripts\Build-Release.ps1` **in the same change**, and update the README
  release-output list. Do **not** add it to `$DistExtras` before the script
  exists — the package helper warns and skips missing extras (noisy, ships
  nothing).
- If a **new** C++ source file is added (rather than extending existing
  runtime/analyzer files), register it in `svg_mb_control_core` in
  `CMakeLists.txt`. Extending `tests/cpp/csv_rows_tests.cpp` or
  `tests/cpp/analyze_report_tests.cpp` needs no CMake change.
- Release publishing (`build-release.ps1` without `-NoPublish`) and any live
  restart remain separate authorization gates.

---

## 0. Evaluation — what changed vs the existing plan/spec

The existing plan is **directionally sound**: it keeps power observational (control
identity stays clean — power never reaches `TempInputs`/setpoint), uses additive
no-false-zero logging, and reuses the FEAT-0006 CPU-watts derivation. Do not
re-architect. Nine corrections matter for implementation:

| # | Existing plan / spec claim | Correction (evidence) |
|---|---|---|
| C1 | "bind fields by name" / "summarize power only when columns present" (plan §2.6, REQ-PWRLOG-05) implied a low-touch, name-bound analyzer change. | Name-binding is real **only** at the CSV-header→struct layer (`analyze_csv.cpp:82-94` `GetField`). The DB ingest is a **versioned hardcoded positional INSERT** (`analyze_ingest_db.cpp:156-175`, column list + `?1..?41`; positional binds `:187-227`) gated by `kSchemaVersion=10` (`analyze_db.h:15`). GPU power = a **v10→v11 schema bump touching ~8 sites** (Track C). |
| C2 | GPU power "summarize like CPU package power" (Σenergy/Σwindow). | **Wrong math.** `nvml_power_mw` is **instantaneous board milliwatts** (`gpu_snapshot.h:95-101`: "NVML board total"), not an accumulating energy counter. Use **mean/p50/p90/max over samples** in a new `SummariseGpuPower`, never `ComputePackagePower`. |
| C3 | §11 default prefers cached cadence because "a per-tick GPU power read is too expensive." | Premise **refuted**: a power read is **one** NVML call (`nvml_loader.cpp:160-163`), not the ~dozen-call `SampleEvidence` bundle (`gpu_sensor_reader.cpp:26-30`). The cached-cadence default rests on a false cost-equivalence. |
| C4 | (implicit) per-tick GPU power is risky because of cost. | Real risk: `ThermalFast` has **zero** NVML calls today (`gpu_probe.cpp:451-457`), so per-tick power is the **first NVML dependency on the hot path**, and absolute NVML latency is not statically knowable. Resolved below to per-tick piggyback with a post-flip measurement gate. |
| C5 | Spec §3/§5 lists `disabled/unavailable/quarantine/validated` as live-loop markers. | The runtime worker emits **only `{disabled, unavailable, quarantine}`** (`amd_reader.cpp:590,598,620,623,637`; comment `:584` "never auto-promotes to validated"). `validated` is applied **post-hoc by a script** (`Capture-EnergySession.ps1`), not the live loop. |
| C6 | Plan §1 / spec §5.2: the safety-revert "drives the env back to disabled" (passive clobber). | The revert is **active**: `Reset-EnergyToDisabled.ps1:60-74` loops up to 120 s after boot/logon and, on seeing the live `quarantine` marker, **stops the task + kills `svg-mb-control.exe` + restarts**. A passive flip is **torn down within 120 s of every boot/logon**. |
| C7 | §13 lists gate 6 (runtime cadence evidence) as a precondition to "buildable work." | Gate 6 (REQ-PWRLOG-04) is **M+R only** (`FEAT-0020.md:198`) and can close **only after a live flip window**. Split acceptance: build-authorize on gates 1-5,7; gate 6 closes post-flip. **MEASUREMENT_GATE.md has no numeric 250 ms envelope** — a pre-flip baseline must be captured as the comparator. |
| C8 | GPU field set fixed at 5 (`gpu_power_sample_id/time_ms/mw/source/acquisition`) but the rationale for sample_id/time_ms is unstated. | sample_id/time_ms carry real meaning for RAPL only because energy is accumulated at ~1 s and mirrored across 250 ms ticks (`SummarisePackagePower` GROUP BY). Under an **instantaneous per-tick** GPU read they are redundant (not wrong). **Resolution (§3.1, open decision):** the plan recommends **keeping all 5** as a cadence-agnostic schema (REQ-PWRLOG-02-compliant + decoupled from the deferred gate-6 measurement), with a 3-field minimal alternative that would require amending REQ-PWRLOG-02. |
| C9 | Refuted operator dead-ends unaddressed. | **Machine-scope env does NOT escape** (User overrides Machine; revert writes User=disabled). **"Set User=enabled and hope" fails** (active restart). Both ruled out (Track D). |

---

## 1. Tracks

Two implementation tracks plus shared analyzer / operator / docs / test work.

- **Track A — CPU side:** enable the already-built FEAT-0006 RAPL package-energy path. **No worker source edit** — operator/profile + docs only.
- **Track B — GPU side:** add new GPU power columns to the control-loop CSV (new code: reader field, snapshot plumbing, CSV writer).
- **Track C — Analyzer:** v10→v11 schema bump for GPU power (CPU columns already at v9/v10).
- **Track D — Operator flip/revert:** reversible profile pair that survives the Safety Revert task.
- **Track E — Docs + traceability.**
- **Track F — Tests.**

---

## 2. Track A — CPU package power (enable existing FEAT-0006)

CPU columns and read path already ship; the only change is making the worker see
`SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` and documenting it. **No worker source edit.**

Reference seams (no edit):
- Env gate (cached at init): `src/hardware/amd_reader.cpp:534-535` + `src/hardware/rapl_energy.h:37-39` (exact `== "enabled"`). Read once via `_dupenv_s` (`env_util.cpp:8-18`) → **flip requires a worker restart**.
- Off-hot-path read: `amd_reader.cpp:837-842` (before PCI mutex, rdmsr-only) + ~1 s self-cadence (`:601-614`), guarded never-throw (`:582-584`).
- Existing CSV columns (verbatim — do not "normalize" prefixes): `cpu_power_sample_id, cpu_power_window_ms, cpu_pkg_energy_delta_uj, cpu_pkg_energy_acquisition` at `src/runtime/runtime_csv_rows.cpp:605-608` (header) / `:665-669` (row).

Marker reality (record in spec, fixes C5): under the flip the live marker becomes
**`quarantine`** on healthy hardware or **`unavailable`** on bin-hash-mismatch/MSR-#GP.
It is **never** `validated` from the live loop.

Cycles stay off: do not set `SVG_MB_CONTROL_CPU_CYCLES_MODE` (default disabled).
Spec §3/§11 already correct on this.

---

## 3. Track B — GPU power (new columns)

### 3.1 Design decision (resolves §11 / D-PWRLOG-1 cadence) — **DECIDED 2026-06-18: 5-field cadence-agnostic**

> **Maintainer decision (2026-06-18):** ship the **5-field cadence-agnostic schema**, read
> per-tick by default. This satisfies REQ-PWRLOG-02 as written (no spec amendment) and keeps
> the schema independent of the gate-6 cadence measurement. The analysis that led here follows.

Two questions are tangled here and must be decided before Track B/C are built:
**(i)** GPU read cadence — per-tick vs bounded-cached; **(ii)** schema field count —
3 vs 5. They interact: a *cached* cadence genuinely needs `gpu_power_sample_id` to
GROUP-BY-dedup repeated instantaneous values, while a *per-tick* read does not.

What the evidence settles:
- A GPU power read is exactly **one** `nvmlDeviceGetPowerUsage` call (`nvml_loader.cpp:160-163`), not the heavy `SampleEvidence` bundle (C3). The cached-cadence default's "too expensive" premise is refuted.
- The value is **instantaneous mW** (`gpu_snapshot.h:95-101`), so per-tick has no accumulation window to mirror (C8).
- **Unresolved by static analysis (C4):** `ThermalFast` is pure-NVAPI today (zero NVML), so per-tick power is the **first hot-path NVML dependency** and absolute NVML latency is not statically knowable. This is exactly what gate 6 (REQ-PWRLOG-04) measures **post-flip** — so the cadence choice cannot be *proven* at planning time.

**Recommended: 5-field, cadence-agnostic schema.** Ship all five fields and read
per-tick by default, but make the schema independent of the cadence decision the gate-6
measurement might overturn:

- `gpu_power_sample_id` — per-tick (shipped): a reader-owned counter that advances only on a fresh successful NVML read (holds on skip/fail); cached: the bounded sample id
- `gpu_power_time_ms` — per-tick (shipped): a reader-owned monotonic clock stamped at the board-power read (`MonotonicGpuReadMs()`); cached: the cached-read timestamp
- `gpu_power_mw` — instantaneous board mW; blank when unavailable (no false zero)
- `gpu_power_source` — `unknown` unless NVML returns nonzero, then `nvml` (`gpu_probe.cpp:188-190`)
- `gpu_power_acquisition` — `disabled` / `unavailable` / `nvml` marker

Why 5-field cadence-agnostic over the leaner 3-field:
1. **Satisfies REQ-PWRLOG-02 as written** ("…additive GPU power fields with sample identity, timestamp, source, acquisition marker…"). The 3-field set would **contradict** that requirement and require amending it (and `test_feature_specs.py` checks structural presence, not requirement-vs-schema semantics, so the contradiction would pass the automated gate and only a human reviewer would catch it).
2. **Decouples the schema from a deferred measurement.** If per-tick later fails gate 6 and forces cached cadence, a 3-field schema needs a *second* version bump (v11→v12) to add `sample_id`; the 5-field schema absorbs the change with no redo.
3. The cost is two columns that are merely redundant (not wrong) under per-tick — and they self-document the cadence, matching the spec's "make repeated values explicit" language.

**Alternative — 3-field minimal (per-tick only).** Leanest steady-state schema, consistent
with the repo's YAGNI lean. Costs: (a) must amend REQ-PWRLOG-02 in Track E with an explicit
justification for dropping sample identity/timestamp; (b) commits to per-tick before gate 6,
so it **should** be de-risked first with a throwaway default-off `nvmlDeviceGetPowerUsage`
latency probe (the established repo pattern — `tools/cpu_msr_validation_probe.cpp`,
`tools/cpu_cycle_counter_probe.cpp`) before the analyzer bump is built.

The rest of this plan is written for the **decided 5-field schema**. (The 3-field minimal
alternative — leaner schema, but requires amending REQ-PWRLOG-02 and a throwaway per-tick
latency probe first — was considered and not chosen.)

### 3.2 Edit sites (GPU)

| What | Location | Edit |
|---|---|---|
| Hot-path GPU sample (temp-only today) | `src/hardware/gpu_reader.cpp:421-456` (`Sample`, `ThermalFast` `:446`); `GpuTempSample` `gpu_reader.h:16-23` | Add `power_mw` + `power_source` to `GpuTempSample`; add the single `get_power_usage` call into `ThermalFast` (or a new `ThermalFastPlusPower` mode), `has_nvml`-guarded. |
| Cheapest probe point | `third_party/nvapi-controller/telemetry/src/gpu_probe.cpp:451-457` (`sample_thermal_fast`) + one-call power read `:184-191` / `nvml_loader.cpp:160-163` | Add the single power read here; keep `has_nvml` guard `:187` and nonzero-gated `power_source` `:188-190`. |
| Snapshot plumb | `src/platform/direct_runtime_snapshot.cpp:106-114` (`MergeGpuTelemetry`) + read site `:204` | Copy new GPU power fields into `RuntimeSnapshot` (mirror `pkg_energy_*` merge `:86-89`). |
| RuntimeSnapshot struct | `src/runtime/runtime_snapshot.h` (`RuntimeGpuSnapshot`) | Add the GPU power fields. |
| Control-loop CSV **header** | `src/runtime/runtime_csv_rows.cpp:584` (`BuildControlLoopCsvHeader`), after the FEAT-0006 block `:613` | Add the 5 GPU power header literals adjacent to the cpu power block — **control-loop only, NOT** the shared `BuildCommonCsvPrefix` (that would also add them to read-loop/evidence-log CSVs). |
| Control-loop CSV **row** | `src/runtime/runtime_csv_rows.cpp:634` (`BuildControlLoopCsvRow`), `:665-678` block | Add matching `AppendCsvFieldIf`/`AppendCsvFieldDouble` (NaN→blank) + `AppendCsvFieldString` calls at the **identical ordinal position** as the header edit. Source `gpu_power_mw`/`_source`/`_acquisition` **and** `gpu_power_sample_id`/`gpu_power_time_ms` from `snapshot.gpu.*` — the reader populates the success-gated counter + `MonotonicGpuReadMs()` timestamp in `GpuReader`'s `FinalizeGpuPowerIdentity`; the row builder does **not** derive them from `tick_count`/`snapshot_time`. |

Drift guard (low-sev): header (`:605-613`) and row (`:665-678`) are hand-aligned in two
separate functions with **no shared schema array and no CSV schema-version token**. Add
GPU columns at the same ordinal position in both; `tests/test_control_loop.py` header
tests catch misordering.

Keep distinct from GPU temp: `gpu_core_c/gpu_memjn_c/gpu_hotspot_c` feed control as
**temperatures** via `GpuControlEnvelopeC` (`channel_evaluator.cpp:439`, `tick_runner.cpp:190`).
The new `gpu_power_mw` (watts) has **no control consumer**; do not route it through `TempInputs`.

---

## 4. Track C — Analyzer ingest + report (honest v10→v11 schema bump)

GPU power is **not** a one-line add. It is a versioned positional-schema change. The CPU
columns already migrated (v8→v9, `analyze_db.cpp:587-613`); reuse that pattern.

| # | Edit site | Change |
|---|---|---|
| 1 | `src/analyze/analyze_db.h:15` | `kSchemaVersion` `10` → `11`. |
| 2 | `src/analyze/analyze_db.cpp:40-83` (`kSchemaSql`) | Add the 5 nullable columns `gpu_power_sample_id` / `gpu_power_time_ms` / `gpu_power_mw` / `gpu_power_source` / `gpu_power_acquisition` to `tick_samples` (fresh DBs). |
| 3 | `src/analyze/analyze_db.cpp` after `:644` (`MigrateSchema`) | Add `if (version <= 10) { ColumnExists-guarded ALTER ADD COLUMN x5; SetSchemaVersion(db,11); }` — the degrade-on-old-DB path. |
| 4 | `src/analyze/analyze_csv.cpp` ~`:297-310` | Add named `GetField` parse calls for the 5 GPU fields (missing column → empty → NULL = degrade-on-missing-CSV-column, `:86-93`). |
| 5 | `src/analyze/analyze_ingest_db.cpp:156-175` + `:187-227` | Extend positional INSERT column list + `?42..?46` placeholders + `BindOptional*` (5 columns). **This is the non-name-bound layer the spec understated (C1).** |
| 6 | New `SummariseGpuPower` near `src/analyze/analyze_report_queries.cpp:469-504` | **Different math from CPU (C2):** mean / p50 / p90 / max over `gpu_power_mw` where nonzero/non-null; reuse the no-false-zero `gpu_power_acquisition` COALESCE-default pattern (`:474-476`). **Do not** Σenergy/Σwindow (GPU mW is instantaneous, not energy). Under per-tick each row is its own sample (no GROUP BY); under a cached cadence, GROUP BY `gpu_power_sample_id` to dedup mirrored values. try/catch degrade for pre-v11 DBs. |
| 7 | `src/analyze/analyze_report.cpp:134` | `data.gpu_power = SummariseGpuPower(db, run_id)`. |
| 8 | `src/analyze/analyze_report_emit.cpp:378` (JSON) + `:534` (text) | New GPU-power report block in **both** emitters. |

CPU watts derivation unchanged (`SummarisePackagePower` + `ComputePackagePower`,
`analyze_report_data.cpp:68-94`); **no second CPU watts column** (REQ-PWRLOG-05).
`system_cpu_*` stays intentionally un-ingested (no change).

---

## 5. Track D — Operator flip / revert (must survive the Safety Revert)

The Safety Revert is **active**, not passive (C6): `Reset-EnergyToDisabled.ps1:60-74`
kills+relaunches the worker within 120 s of every boot/logon when it sees `quarantine`.
Registered AtStartup+AtLogon by `Schedule-EnergySessions.ps1:79-89`. There is **no scripted
teardown** today (reversibility gap).

Rejected (refuted — do not attempt):
- Machine-scope env — User scope overrides Machine; revert writes User=`disabled` (C9).
- "Set User=enabled and hope" — actively torn down within 120 s (C6).

Recommended reversible profile pair (new repo-owned scripts):
- `Set-EnergyLoggingProfile.ps1 -Enable`: `Disable-ScheduledTask -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Energy Safety Revert'` (reversible via `Enable-ScheduledTask`, reboot-durable) → set User `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` (cycles left disabled) → restart via the documented `--restart` path (`Install-SVG-MB-ControlScheduledTask.ps1:98-101`) or `Restart-WorkerTree` (`Capture-EnergySession.ps1:117`).
- `Set-EnergyLoggingProfile.ps1 -Disable`: re-`Enable-ScheduledTask` the Safety Revert, set User var `disabled`, restart.

Reuse the enable/restart pattern from `Capture-EnergySession.ps1:274-277` but **do not
self-revert** (that script is time-boxed; this profile is persistent).

Alternative (more code, preserves crash-safety for capture runs): make
`Reset-EnergyToDisabled.ps1` **sentinel-aware** — skip force-OFF + restart when a
`logging-profile-on` sentinel exists, so interrupted *capture* sessions still fail safe
while the standard profile persists. Recommend the Disable/Enable pair for v1; offer
sentinel as the maintainer's choice.

Governance inversion (must be an explicit maintainer decision in D-PWRLOG-1): the Safety
Revert encodes FEAT-0006's premise "never leave energy enabled after a restart until the
gate passes." FEAT-0020 wants `enabled` as steady state — this **inverts** that premise.
Record it explicitly; do not silently override. The worker still won't promote past
`quarantine` (`amd_reader.cpp:584,637`), so FEAT-0006's evidence-quality stamp is
unaffected; only the boot-time-OFF guarantee changes.

---

## 6. Track E — Docs + traceability (lockstep)

- `docs/features/FEAT-0020-...md` — fix §3/§5 marker set to `{disabled,unavailable,quarantine}` (drop live `validated`, C5); change §11 GPU cadence default to **per-tick read with a cadence-agnostic 5-field schema** and the corrected rationale (C3/C4); confirm §7 keeps the 5-field set so it stays REQ-PWRLOG-02-compliant; fix §13 so gate 6 closes **post-flip** (C7); fill verification log post-flip. *(If the 3-field alternative is chosen instead, this is where REQ-PWRLOG-02 must be amended to drop sample identity/timestamp with justification.)*
- `docs/power-logging-flip-plan-2026-06-18.md` (D-PWRLOG-1) — records the cadence decision, the GPU mean/percentile math (not Σenergy), the operator Disable/Enable pair, and the governance inversion; Status `Current` closes gate 3.
- `README.md` — standard power-logging profile + safe verification commands (`Set-EnergyLoggingProfile.ps1`).
- `docs/RUNTIME_HOME.md` — new additive GPU power CSV fields; note no `current_state.json` mirror in v1.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` — document the 5 GPU columns + correct the marker enum so it does not list live `validated`.
- `docs/TRACEABILITY.md` — add REQ-PWRLOG-01..06 rows mapped to tests / M-evidence / review.
- `docs/CONTROL_PIPELINE_MATH.md` — **no change** (additive observational columns fall outside its update trigger `:21-25`; power never reaches `TempInputs`). Confirmed by the control-identity finding.
- `docs/features/README.md` registry — add FEAT-0020 row using `**FEAT-0020**`/order-number first cell per the index gate-safety rule.

---

## 7. Track F — Test plan

| Area | Test | Asserts |
|---|---|---|
| CSV header/row | `tests/test_control_loop.py` | New 5 GPU columns present at the expected ordinal position; blank `gpu_power_mw` + marker when unavailable (no false zero). |
| Analyzer old-archive degrade | `tests/test_analyze_ingest.py` (mirror `ENERGY_FIELDS`/`CYCLE_FIELDS` `:100-120`, `:1222`, `:1260`) | Pre-v11 CSV/DB ingests; report degrades to unavailable; `test_ingest_migrates_v9_db_to_current_schema`; `schema_version=='11'` (`:572-576`). |
| GPU summary math | new analyzer report test | `SummariseGpuPower` produces **mean/percentile**, result **≠** any Σenergy/Σwindow integral on a known instantaneous-mW fixture (catches C2 silently-wrong number). |
| Control identity | C++ test / review | Power fields not read by `EvaluateChannel`/boost/write path; `power_anticipation.h` stays unreferenced by `src/control`/`src/runtime` (only `tests/cpp/power_anticipation_tests.cpp`). |
| Script/workflow | script test or dry-run review | `Set-EnergyLoggingProfile.ps1 -Enable/-Disable` toggles the User var + Disable/Enable-ScheduledTask; revert coexistence. |
| Gate | `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` + `python tests/test_feature_specs.py` (5/5) | Full local CI green before any commit. |

---

## 8. Gate sequencing (explicit — not all gates close before code)

Close at planning/impl time (build-authorize on these):
- Gate 1 (problem) done. Gate 2 (invariants) done. Gate 4 (REQ IDs) done. Gate 7 (doctrine) done.
- Gate 3 (decision record Current) — closes when D-PWRLOG-1 is updated per §6 and marked Current.
- Gate 5 (verification mapped + TRACEABILITY) — closes when traceability rows land.

Closes ONLY after a live, separately-authorized flip window:
- **Gate 6 / REQ-PWRLOG-04** (M+R, `FEAT-0020.md:198`): runtime evidence that the added GPU NVML read + enabled CPU energy read do **not** move the 250 ms baseline. Proof surface (`loop_work_duration_ms`, `loop_slip_ms`, `loop_overrun`, `process_cpu_pct`, `runtime_csv_rows.cpp:590-603`) **only populates at runtime**. **`loop_work_duration_ms` is the sensitive column** (the fixed-start-period sleep, `control_scheduler.cpp:158-193`, masks sub-ms cost in `loop_slip_ms`).
- **Pre-flip baseline (gap fix):** MEASUREMENT_GATE.md has **no numeric 250 ms envelope** — its only numbers are historical 50 ms data marked do-not-use. Capture a **same-machine/same-build pre-flip 250 ms baseline** as the named comparator, then a post-flip capture. Without it, "within the envelope" is subjective.
- Optional: wire the instrumented `SampleDirectRuntimeSnapshot` overload (the 5-arg `nullptr`-timing variant is used today, `tick_runner.cpp:157`→`direct_runtime_snapshot.cpp:157-158`) to measure the GPU/energy read cost directly rather than inferring from whole-tick duration.

Restate spec §13 so it does **not** imply gate 6 closes before code. Use the project's
two-phase pattern ("Accepted ≠ build-authorized", per FEAT-0006).

---

## 9. Ordered, checkable steps

1. [x] Update `FEAT-0020` spec: marker set `{disabled,unavailable,quarantine}`; §11 → per-tick read + 5-field cadence-agnostic schema; §7 → keep 5 fields (REQ-PWRLOG-02-compliant); §13 wording (gate 6 post-flip). (Track E)
2. [x] Update D-PWRLOG-1: cadence decision, GPU mean/percentile math, operator Disable/Enable pair, governance inversion; mark **Current** (closes gate 3).
3. [x] Add REQ-PWRLOG rows to `docs/TRACEABILITY.md`; add FEAT-0020 registry row (closes gate 5).
4. [x] Verify `RuntimeSnapshot`/`GpuTempSample` structs; add `power_mw`/`power_source` to the GPU reader and plumb `gpu_power_mw`/`_source`/`_acquisition` onto `RuntimeSnapshot`. (Track B)
5. [x] Add the single NVML `get_power_usage` read into the `ThermalFast` GPU path via the board-power-only helper (no per-tick NVAPI topology read). (Track B)
6. [x] Plumb power fields through `MergeGpuTelemetry` (`direct_runtime_snapshot.cpp`). (Track B)
7. [x] Add 5 GPU columns to control-loop header **and** row at identical ordinal positions. (Track B)
8. [x] Analyzer v10→v11: bump `kSchemaVersion`, `kSchemaSql`, migration ladder, CSV parse, positional INSERT, `SummariseGpuPower` (mean/percentile), report assembly, JSON+text emit. (Track C)
9. [x] Add `Set-EnergyLoggingProfile.ps1 -Enable/-Disable` (Disable/Enable Safety Revert task + User env + restart) **and packaging** (`$DistExtras`/README). (Track D)
10. [x] Tests: CSV header/row, analyzer old-archive degrade + migration + GPU-math-≠-integral, control-identity (response source stays `primary_curve`), and operator dry-run workflow. (Track F)
11. [x] Update `README.md`, `RUNTIME_HOME.md`, `RUNTIME_LOGGING_AND_EVALUATION.md`. (Track E)
12. [x] Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` + `python tests/test_feature_specs.py` → all green (covered by full local CI; CTest `13/13`, hermetic Python `169/169`).
13. [x] Release authorization granted; PR [#20](https://github.com/espensev/SVG-MB-Control/pull/20) opened.
14. [x] Explicit live-runtime authorization granted 2026-06-18 (maintainer chose all-at-once: deploy + enable).
15. [x] Captured pre-flip 250 ms baseline → `Build-Release.ps1` deploy of `1ea44c7` → `Set-EnergyLoggingProfile.ps1 -Enable` → captured post-flip window: CPU energy sample ids present, GPU power present, loop timing within envelope (mean unchanged; under-load max 14.6 ms), `channel0_response_source=primary_curve`, old archives still ingest (55 runs under v11). Results: `docs/feat-0020-live-flip-validation-results-2026-06-18.md`.
16. [x] Filled §14 verification log; gate 6 closed.

---

## 10. Stop points / authorization needed

- **Build/release (`B`) gate:** `Build-Release.ps1` / live-deploy is a separate authorization from local `Test-LocalCI`. Do not deploy on local-CI green alone.
- **Live-runtime authorization (mandatory before step 15):** any worker restart, env flip, or `Disable-ScheduledTask 'SVG-MB Energy Safety Revert'` touches the live controller and the FEAT-0006 boot-OFF guarantee — requires explicit maintainer sign-off (`AGENTS.md` §Live Runtime Safety; spec §4/§5). The governance inversion (Track D) must be an **explicit recorded decision**, not a silent override.
- **Gate 6 cannot close before steps 14-16.** Do not mark FEAT-0020 fully verified on build/CI evidence alone.

---

### Key files

- Spec: `docs/features/FEAT-0020-standard-control-loop-power-logging.md`
- Decision: `docs/power-logging-flip-plan-2026-06-18.md`
- CSV writer: `src/runtime/runtime_csv_rows.cpp`
- GPU reader: `src/hardware/gpu_reader.cpp` + `third_party/nvapi-controller/telemetry/src/gpu_probe.cpp` + `third_party/nvapi-controller/src/nvml_loader.cpp`
- Snapshot: `src/platform/direct_runtime_snapshot.cpp`
- Worker env gate: `src/hardware/amd_reader.cpp`, `src/hardware/rapl_energy.h`, `src/platform/env_util.cpp`
- Analyzer: `src/analyze/analyze_db.{h,cpp}`, `analyze_csv.cpp`, `analyze_ingest_db.cpp`, `analyze_report_queries.cpp`, `analyze_report_data.cpp`, `analyze_report.cpp`, `analyze_report_emit.cpp`
- Scripts: `scripts/Reset-EnergyToDisabled.ps1`, `scripts/Schedule-EnergySessions.ps1`, `scripts/Capture-EnergySession.ps1`, `scripts/Install-SVG-MB-ControlScheduledTask.ps1`
- Control identity: `src/control/channel_evaluator.cpp`, `src/policy/control_policy.h`, `src/control/power_anticipation.h` (keep unwired), `docs/CONTROL_PIPELINE_MATH.md`
