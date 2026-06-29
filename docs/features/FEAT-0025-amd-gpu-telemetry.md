# FEAT-0025: AMD GPU telemetry backend

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-22
**Namespace:** `REQ-AMDGPU-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/features/FEAT-0013-source-aware-primary-dropout-safe-mode.md`,
`docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`
**Purpose:** add a read-only AMD (Radeon) GPU telemetry backend so the existing
GPU control envelope and GPU logging slices produce live data on an AMD GPU
machine, instead of staying empty because the only GPU backend is NVIDIA.

## 1. Summary

Today the GPU telemetry stack is NVIDIA-only: `GpuReader` wraps the vendored
`gpu_telemetry` NVAPI/NVML slice behind the `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED`
compile gate (`src/hardware/gpu_reader.cpp:1-5`). On a machine whose only GPU is
an AMD Radeon part, `GpuReader::available()` stays false, `Sample()` returns
`available=false`, the GPU control envelope (`GpuControlEnvelopeC`,
`src/control/channel_evaluator.cpp:449`) receives no GPU temperature, and the
FEAT-0020 power / FEAT-0021 context CSV slices stay `disabled`/`unavailable`.
This feature adds a second, read-only AMD GPU backend behind `GpuReader` that
populates the same `GpuTempSample` / `GpuEvidenceSample` structures, so the
existing envelope and logging paths work on an AMD GPU without changing the
NVIDIA path or the control math. AMD GPU temperature is treated as
logging-only until the §12 measurement gate authorizes it as a control input.

## 2. Problem & motivation  *(promotion gate 1)*

The shipped scope (`README.md:10-20`) states "optional direct NVIDIA telemetry
through the vendored `gpu_telemetry` slice"; there is no AMD GPU path. The code
gap is concrete:

- `src/hardware/gpu_reader.cpp:3-5` only includes
  `gpu_telemetry/gpu_sensor_reader.h` (NVAPI/NVML) under
  `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED`; there is no AMD backend include or
  compile gate.
- `GpuTempSample.power_source` is `"unknown"` unless an NVML read succeeds, then
  `"nvml"`; `*_acquisition` markers are `disabled` / `unavailable` / `nvml`
  only (`src/hardware/gpu_reader.h:24-44`). No AMD provenance value exists.
- `GpuControlEnvelopeC` (`src/control/channel_evaluator.cpp:449-455`) computes
  `max(core_c, memjn_c, hotspot_c>0)` from `RuntimeGpuSnapshot`, which is filled
  from `GpuReader::Sample()`. With no AMD source, an AMD-GPU machine feeds the
  envelope nothing and the case fans respond to CPU temperature alone.

The operator-visible effect on an AMD GPU machine: the GPU-envelope curves and
the GPU power/context columns that FEAT-0020/0021 added carry no signal, so the
cooling strategy's GPU-airflow lanes (`docs/COOLING_STRATEGY.md`) cannot react to
GPU heat. The motivating hardware is an AMD Radeon GPU paired with a Ryzen 9
9950X3D (operator-supplied iCUE telemetry screenshot, 2026-06-22), where iCUE
reports Radeon Temp #1/#2, Load, Memory Load, and Video Engine but
`svg-mb-control` records no GPU data.

## 3. Goals & non-goals

**Goals**

- Add a read-only AMD GPU telemetry backend (proposed source: AMD ADLX,
  dynamically loaded) that produces GPU core/hotspot/memory-junction
  temperatures into the existing `GpuTempSample`.
- Make `GpuReader` vendor-selecting: when the AMD backend initializes and
  enumerates a GPU, `available()` is true and `Sample()` returns AMD
  temperatures through the same struct the NVIDIA path uses.
- Reuse the existing FEAT-0020 GPU power slice and FEAT-0021 GPU context slice
  for AMD where ADLX exposes the equivalent metrics; keep them logging-only.
- Extend the provenance markers additively so an AMD sample is distinguishable
  from an NVIDIA sample and from "unavailable", preserving the existing
  no-false-zero rule.
- Keep the AMD GPU temperature out of the control envelope until the §12
  measurement gate (AMD temperature validity plus 250 ms loop-timing impact) is
  satisfied; ship it logging-only first if the gate is not yet met.

**Non-goals**

- No change to `GpuControlEnvelopeC` math, CPU/blend behavior, curves, cadence,
  channels, write policy, breaker, or restore behavior.
- No GPU fan **control** (writing GPU fan duty); this feature is GPU sensor
  read-only, consistent with the repo owning case/motherboard fans only.
- No new sibling-repo dependency, HWiNFO, vendor service, or subprocess bridge
  (`AGENTS.md` §Repo Boundary).
- No UI work (`docs/MEASUREMENT_GATE.md`); the operator surface stays CLI/CSV.
- No simultaneous multi-vendor fusion in v1 (one active GPU backend per process);
  dual-GPU NVIDIA+AMD blending is out of scope.
- No replacement or refactor of the NVIDIA path's behavior; the NVIDIA backend
  output must stay byte-for-byte unchanged when it is the selected backend.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The AMD backend is vendored under `third_party/` and loaded in-process (dynamic load of a redistributable, mirroring the NVAPI/NVML and PawnIO pattern), with pinned provenance + SHA-256. No sibling repo, no subprocess sensor adapter. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The backend is sensor-read-only. It writes no GPU or fan duty. The spec itself is non-actuating; capturing live measurement evidence requires separate live-runtime authorization. |
| Shipped 250 ms cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | Adding GPU reads to the per-tick path must keep `loop_work_duration_ms` and process-resource metrics inside the current envelope; AMD temperature may drive the envelope only after the §12 evidence, otherwise the backend stays logging-only. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | When AMD temps are promoted to a control input they enter through the unchanged `GpuControlEnvelopeC` rule; no new term is added. Until then GPU power/context stay logging-only exactly as FEAT-0020/0021 define. |
| Runtime sidecar / status / CSV / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md` | v1 reuses the existing FEAT-0020/0021 columns and only widens the value domain of `*_source` / `*_acquisition` (adds an `adlx` value). No column is removed or renamed; old archives keep ingesting by header name. |
| Source-aware dropout / safe-mode behavior is preserved | `docs/features/FEAT-0013-source-aware-primary-dropout-safe-mode.md` | An unavailable or implausible AMD read behaves as GPU-absent and reuses the existing source-aware dropout / safe-mode path; it must never let a stale/false AMD value drive fans. |

## 5. Behavior specification

### Backend and selection seam

The implementation must add an AMD GPU telemetry backend behind a new compile
gate (proposed `SVG_MB_CONTROL_GPU_TELEMETRY_AMD_ENABLED`), parallel to the
existing `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED` NVIDIA gate. `GpuReader`
(`src/hardware/gpu_reader.{h,cpp}`) must become vendor-selecting: at
construction it attempts to initialize an available backend and records which
one is active. The seam keeps the public `GpuReader` interface
(`available()`, `init_warning()`, `Sample()`, `SampleEvidence()`) unchanged so
no call site in `src/control/` or `src/runtime/` changes shape.

Backend selection precedence (proposed default, to be settled in §9): when both
backends are compiled and both enumerate a GPU, prefer the NVIDIA backend (the
existing measured path) and record the AMD backend as present-but-inactive; a
build that compiles only the AMD gate selects AMD. Exactly one backend is active
per process for v1. The active vendor must be observable through `gpu_name` and
the provenance markers below.

### Temperature sample

When the AMD backend is active and a GPU is enumerated, `Sample()` must return a
`GpuTempSample` with `available=true` and the AMD-reported temperatures mapped
onto the existing fields: GPU edge/core into `core_c`, memory-junction into
`memjn_c` (0.0 when the part has no memory-junction sensor), and hotspot/junction
into `hotspot_c` (0.0 when absent, matching the existing "0.0 when sensor absent"
contract in `src/hardware/gpu_reader.h:21`). `gpu_name` must carry the AMD device
name. On any read failure the held sample must have `available=false` and
`last_warning` populated, exactly like the NVIDIA path.

### Control envelope (gated)

The AMD temperatures flow into the control envelope only through the unchanged
`GpuControlEnvelopeC` rule (`src/control/channel_evaluator.cpp:449`); the feature
must not add or change envelope math. Because routing a new sensor into the
envelope makes AMD temperature a **control input**, it must stay logging-only
(envelope contribution suppressed, i.e. treated as GPU-absent) until the §12
measurement gate is met. The decision record (§9) must state the explicit
ship-order: logging-only first, control-input second after evidence.

### Logging-only power and context

Where ADLX exposes board/GPU power and workload context (utilization, clocks,
VRAM), the implementation should populate the existing FEAT-0020 power slice and
FEAT-0021 context slice from the AMD backend, keeping them logging-only and
preserving provenance. The same no-false-zero rule applies: a numeric field is
blank unless a fresh nonzero read succeeded, and `*_sample_id` advances only on
such a read.

### Provenance markers

The marker value domains must be widened additively. `power_source` /
`context_acquisition` / `power_acquisition` gain an AMD value (proposed
`"adlx"`); `disabled` / `unavailable` keep their meaning. A single sample must
carry exactly one vendor marker (a sample is `nvml` or `adlx`, never both). Old
archives that only ever recorded `nvml` / `unavailable` / `disabled` must remain
valid.

### Fallback / safe behavior

If the AMD backend fails to initialize, enumerates no GPU, or returns an
implausible reading, `GpuReader` must behave exactly as it does for an absent
NVIDIA GPU: `available()` false (or `Sample().available=false`), the envelope
receives nothing, and the existing FEAT-0013 source-aware dropout / safe-mode
mechanism governs any channel that depends on a GPU primary source. A stale or
out-of-range AMD value must never drive fan duty.

### Hermetic testability

The existing simulation hook `SVG_MB_CONTROL_SIM_GPU_MODE`
(`src/hardware/gpu_reader.cpp:38-51`) must be extended so a simulated AMD sample
(vendor marker `adlx`, synthetic temperatures/power/context) can be produced
without AMD hardware, so the C++ smoke / pytest lanes in
`.\scripts\Test-LocalCI.ps1` exercise the AMD path on CI hosts that have no
Radeon GPU.

## 6. Requirements  *(promotion gate 4 — assign IDs only after the design decision picks a direction)*

| ID | Requirement |
|---|---|
| REQ-AMDGPU-01 | The AMD GPU backend must be read-only and standalone: vendored under `third_party/` with pinned provenance + SHA-256, dynamically loaded in-process (no sibling repo, no subprocess sensor adapter, no third-party service), behind its own compile gate, and the normal release configure path must build reproducibly offline. |
| REQ-AMDGPU-02 | `GpuReader` must select one active backend at construction and, when the AMD backend is active and a GPU is enumerated, return a `GpuTempSample` with `available=true`, AMD `core_c`/`memjn_c`/`hotspot_c` mapped per §5, and `gpu_name` set; the public `GpuReader` interface and the NVIDIA-path output must stay unchanged, and exactly one backend is active per process. |
| REQ-AMDGPU-03 | AMD GPU temperature must reach fan control only through the unchanged `GpuControlEnvelopeC` rule, and must stay logging-only (no envelope contribution) until the §12 measurement-gate evidence (AMD temperature validity plus 250 ms loop-timing/process-resource impact) is captured; promoting it to a control input must not change the envelope math, CPU/blend behavior, curves, cadence, channels, write policy, breaker, or restore behavior. |
| REQ-AMDGPU-04 | Provenance markers must be widened additively to distinguish an AMD sample (proposed `adlx`) from `nvml` / `unavailable` / `disabled`; a sample carries exactly one vendor marker; the no-false-zero rule holds (blank numeric plus marker, `*_sample_id` advances only on a fresh read); and archives recorded before this feature must continue to ingest by header name. |
| REQ-AMDGPU-05 | AMD GPU board/GPU power and workload context, where ADLX exposes them, must reuse the existing FEAT-0020 power and FEAT-0021 context CSV slices as logging-only signals; the analyzer must bind them by header name, summarize them only when present, and degrade older archives as unavailable, with no schema-version redo required for v1 (or the bump documented at implementation in `docs/RUNTIME_LOGGING_AND_EVALUATION.md`). |
| REQ-AMDGPU-06 | An unavailable, failed, or implausible AMD read must behave as GPU-absent and reuse the FEAT-0013 source-aware dropout / safe-mode path so a stale/false AMD value never drives fan duty; and the `SVG_MB_CONTROL_SIM_GPU_MODE` hook must be able to produce a simulated AMD sample so `.\scripts\Test-LocalCI.ps1` exercises the AMD path with no Radeon hardware present. |

IDs come from this feature's `REQ-AMDGPU-*` namespace, reserved in the registry
in `README.md`.

## 7. Data / schema deltas

- New/changed fields: none added in v1. The `*_source` / `*_acquisition` value
  domains gain an `adlx` value (additive enumerated value, not a new column).
- Config impact (`config/control.*.json`, `config/machines/*.json`): none for
  v1. A future machine policy may name the AMD GPU as a source; that is out of
  scope here.
- Schema/version impact: none for the control-loop CSV header or analyzer schema
  in v1 (existing FEAT-0020/0021 columns are reused). If ADLX exposes a metric
  with no existing column and the implementation chooses to log it, that column
  and any analyzer schema bump must be added at implementation and recorded in
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` and this spec first.
- Backward compatibility: no existing runtime-home file, archive, or config may
  become invalid; the only schema motion is widening two marker value domains.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand or flag is required by this spec. `--show-config` and
  `--status` already surface GPU availability indirectly; if the implementation
  adds an explicit active-GPU-vendor field to `--status` / `current_state.json`,
  that field must be additive and documented in `docs/RUNTIME_HOME.md` and this
  spec before it ships.
- Build surface: a new compile gate (proposed `SVG_MB_CONTROL_GPU_TELEMETRY_AMD_ENABLED`)
  in `CMakeLists.txt`, defaulted so the existing NVIDIA-only and no-GPU builds
  are unchanged.
- `README.md` Scope and `docs/COOLING_STRATEGY.md` must be updated **at
  implementation** to state AMD GPU telemetry as current behavior; per
  `CLAUDE.md` this spec does not pre-announce it in those operator docs as if it
  already exists.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

This repo records decisions as dated files in `docs/`. The AMD-API choice and the
control-input gating must be settled and promoted to Current before
implementation.

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`../amd-gpu-telemetry-decision-2026-06-22.md`](../amd-gpu-telemetry-decision-2026-06-22.md) (D-AMDGPU-1) | Which AMD telemetry API (ADLX dynamically loaded, read-only, vendored vs. legacy ADL Overdrive vs. another in-process source); the `GpuReader` vendor-selection seam and precedence when both backends are built; the additive `adlx` provenance marker; reuse of the FEAT-0020/0021 logging slices; and the ship-order gate that AMD temperature stays logging-only until §12 evidence authorizes it as a control input. | Proposed |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-AMDGPU-01 | B, R | Release configure/build with the AMD gate stays offline-reproducible (`.\build-release.ps1` / `scripts\Build-Release.ps1`); review the vendored backend has pinned provenance + SHA-256 like `third_party/pawnio/README.md` and is loaded in-process with no sibling-repo/subprocess dependency. |
| REQ-AMDGPU-02 | T, R | C++ unit/smoke test (via `SVG_MB_CONTROL_SIM_GPU_MODE` AMD mode) that `GpuReader` selects the AMD backend, returns `available=true` with mapped temperatures and `gpu_name`; review the public interface and the NVIDIA-path output are unchanged and exactly one backend is active. |
| REQ-AMDGPU-03 | T, M, R | Test that AMD temps stay out of the envelope until enabled; review `GpuControlEnvelopeC` math and `docs/CONTROL_PIPELINE_MATH.md` are unchanged; manual runtime measurement of AMD temperature validity and 250 ms loop-timing/process-resource impact before any control-input promotion (`AGENTS.md` §Live Runtime Safety, `docs/MEASUREMENT_GATE.md`). |
| REQ-AMDGPU-04 | T, R | CSV header/row test that an AMD sample emits the `adlx` marker with no false zero and a single vendor marker; analyzer ingest test that older `nvml`/`unavailable` archives still bind by header name. |
| REQ-AMDGPU-05 | T, R | Analyzer ingest/report test that AMD power/context reuse the FEAT-0020/0021 columns, summarize only when present, and degrade older archives; review no new schema version is introduced in v1. |
| REQ-AMDGPU-06 | T, R | Test that an unavailable/implausible AMD read trips the FEAT-0013 source-aware dropout / safe-mode path and never writes fan duty from a stale value; review the `SVG_MB_CONTROL_SIM_GPU_MODE` AMD hook lets `.\scripts\Test-LocalCI.ps1` run without Radeon hardware. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default / lean |
|---|---|---|
| AMD API choice: ADLX (dynamic-load, read-only) vs legacy ADL Overdrive vs another in-process source | implementation (settled in §9 D-AMDGPU-1) | **Lean: ADLX**, dynamically loaded and vendored, mirroring the NVAPI/NVML in-process pattern. |
| Backend precedence when both NVIDIA and AMD backends are built and both enumerate a GPU | implementation | Prefer the existing measured NVIDIA path; record AMD as present-but-inactive. |
| Target GPU class: discrete Radeon vs the Ryzen iGPU | measurement | Support whichever ADLX enumerates; a Ryzen iGPU's heat is already in the CPU package, so its envelope value is limited — log first, decide control-input relevance from §12 evidence. |
| Whether to add an explicit active-GPU-vendor status field | implementation | Do not add for v1; vendor is observable via `gpu_name` + markers. Add additively later if operators need it. |
| Whether AMD power/context get any non-FEAT-0020/0021 columns | implementation | No; reuse existing columns. Any new column requires a documented schema bump. |

## 12. Measurement gate & dependencies

- **Measurement gate:** this feature crosses `docs/MEASUREMENT_GATE.md` in two
  ways. (1) It adds per-tick GPU reads, so the added read cost must be shown to
  keep the shipped 250 ms loop-timing/process-resource envelope (same evidence
  shape as FEAT-0020 REQ-PWRLOG-04). (2) Routing AMD temperature into
  `GpuControlEnvelopeC` makes it a control input, which must not be enabled until
  on-hardware evidence shows the AMD temperature is valid and stable; until then
  the backend ships logging-only. Both are captured under explicit live-runtime
  authorization.
- **Depends on:** the existing `GpuReader` seam; FEAT-0020 GPU power slice;
  FEAT-0021 GPU context slice; FEAT-0013 source-aware dropout / safe-mode;
  analyzer ingest/report header-name binding.
- **Build/test impact:** new vendored AMD backend + compile gate; `GpuReader`
  vendor-selection seam; `SVG_MB_CONTROL_SIM_GPU_MODE` AMD simulation; C++ smoke
  / pytest coverage; analyzer ingest tests for the `adlx` marker and old
  archives. No `docs/CONTROL_PIPELINE_MATH.md` change unless/until AMD temp is
  promoted to a control input through the unchanged envelope rule.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 cites `README.md:10-20`, `src/hardware/gpu_reader.{h,cpp}`, `src/control/channel_evaluator.cpp:449`, and operator iCUE telemetry).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9 D-AMDGPU-1 is **Proposed**, not yet Current; the AMD-API choice and control-input gating are still open).
- [x] 4. Concrete `REQ-AMDGPU-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [ ] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline — the §12 loop-timing and AMD-temp-validity evidence is owed before AMD temperature may drive fans; the AMD API/vendoring must still be confirmed standalone at implementation.
- [x] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; proposed behavior is labeled proposed; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

The point of writing this spec in advance: after implementation, confirm each
requirement against the running controller and the cited contract. Date each
check; link the test run, build, commit, or runtime-evidence file. Keep
`docs/TRACEABILITY.md` aligned with the final result.

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-AMDGPU-01 | pending | not yet built — Draft spec; B/R planned per §10 | |
| REQ-AMDGPU-02 | pending | not yet built — Draft spec; T/R planned per §10 | |
| REQ-AMDGPU-03 | pending | not yet built — Draft spec; T/M/R planned per §10 (M owed per §12) | |
| REQ-AMDGPU-04 | pending | not yet built — Draft spec; T/R planned per §10 | |
| REQ-AMDGPU-05 | pending | not yet built — Draft spec; T/R planned per §10 | |
| REQ-AMDGPU-06 | pending | not yet built — Draft spec; T/R planned per §10 | |

**Spec vs. implementation deltas:** none yet; this spec is Draft and not
implemented. Record build-time deltas here, refresh the cited contract docs per
`AGENTS.md` §Change Checklist, and bump **Updated** when implementation lands.
