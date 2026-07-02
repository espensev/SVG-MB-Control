# AMD GPU Telemetry Decision - 2026-06-22

Status: **Proposed** (not yet Current). This record settles the direction for
`docs/features/FEAT-0025-amd-gpu-telemetry.md` (D-AMDGPU-1). It is the
direction-setting decision the FEAT-0025 §9 gate depends on; the feature must not
be implemented until this is promoted to Current.

## Problem

The GPU telemetry stack is NVIDIA-only. `GpuReader` wraps the vendored
`gpu_telemetry` NVAPI/NVML slice behind `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED`
(`src/hardware/gpu_reader.cpp:1-5`), and the only provenance markers are
`nvml` / `unavailable` / `disabled` (`src/hardware/gpu_reader.h:24-44`). On a
machine whose only GPU is an AMD Radeon part, `GpuReader::available()` is false,
the GPU control envelope (`GpuControlEnvelopeC`,
`src/control/channel_evaluator.cpp:449`) receives no GPU temperature, and the
FEAT-0020 power / FEAT-0021 context CSV slices stay empty. The motivating machine
is an AMD Radeon GPU with a Ryzen 9 9950X3D (operator iCUE telemetry,
2026-06-22).

## Options considered

1. **AMD ADLX, dynamically loaded, vendored, read-only (lean).** ADLX is AMD's
   current GPU telemetry/control SDK and exposes GPU temperature
   (edge/hotspot/memory), board and GPU power, fan speed, clocks, utilization,
   and VRAM. Using only its read interfaces matches the repo's existing
   in-process, dynamically loaded, vendored pattern (NVAPI/NVML, PawnIO `.bin`
   modules) and keeps the repo standalone. Cost: a new vendored dependency plus
   provenance/SHA-256 pinning, and ADLX's interface-object initialization in a
   new backend wrapper.
2. **Legacy AMD ADL Overdrive (`atiadlxx.dll`).** Older, widely present, simpler
   C API. Cost: ADL is superseded by ADLX for newer Radeon parts and exposes a
   narrower/less consistent metric set; AMD positions ADLX as the path for new
   work. Higher risk of missing metrics on current GPUs.
3. **Windows performance counters / D3DKMT / WMI.** No extra vendored SDK, but
   the available GPU thermal/power coverage is inconsistent across drivers and
   parts, and it does not match the precision the envelope expects. Rejected as a
   primary source.
4. **Reuse a sibling repo or an external sensor service (e.g., HWiNFO).**
   Rejected outright: violates `AGENTS.md` §Repo Boundary (standalone, no
   subprocess sensor adapter, no sibling-repo runtime dependency).

## Proposed decision

- **API:** AMD ADLX, **read-only**, **dynamically loaded**, **vendored** under
  `third_party/` with pinned provenance + SHA-256 (same hygiene as
  `third_party/pawnio/README.md` and the single-header `nlohmann-json`). The
  normal release configure path must build offline.
- **Seam:** make `GpuReader` vendor-selecting behind its existing public
  interface (`available()`, `init_warning()`, `Sample()`, `SampleEvidence()`).
  Add a parallel compile gate (proposed `SVG_MB_CONTROL_GPU_TELEMETRY_AMD_ENABLED`)
  beside `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED`. Exactly one backend is active
  per process in v1.
- **Precedence:** when both backends are compiled and both enumerate a GPU,
  prefer the existing measured NVIDIA path and record the AMD backend as
  present-but-inactive. An AMD-only build selects AMD.
- **Markers:** widen the `*_source` / `*_acquisition` value domains additively
  with an `adlx` value. A sample carries exactly one vendor marker. Preserve the
  no-false-zero rule and the `*_sample_id`-advances-only-on-fresh-read semantics.
- **Logging slices:** reuse the FEAT-0020 GPU power slice and FEAT-0021 GPU
  context slice for AMD where ADLX exposes the equivalent metrics; keep them
  logging-only. No new CSV column or analyzer schema version in v1.
- **Ship order (the key safety gate):** ship the AMD backend **logging-only
  first**. AMD temperature is routed into `GpuControlEnvelopeC` (making it a
  control input) **only after** the FEAT-0025 §12 measurement evidence: (a) the
  added per-tick read keeps the shipped 250 ms loop-timing/process-resource
  envelope, and (b) on-hardware AMD temperature is shown valid and stable. Until
  then the envelope contribution is suppressed and a GPU-primary channel relies
  on the FEAT-0013 source-aware dropout / safe-mode path.

## Why logging-only first

Routing a new sensor into the control envelope changes what drives fan duty.
The repo's measurement gate (`docs/MEASUREMENT_GATE.md`) and Live Runtime Safety
contract (`AGENTS.md`) require evidence before a new input moves the baseline.
A logging-only first step lets the AMD reading be validated against iCUE / known
load on real hardware with zero control risk, then promoted through the unchanged
`GpuControlEnvelopeC` rule with no control-math change.

## What this decision does not settle

- The exact ADLX redistributable version and its pinned SHA-256 (recorded at
  implementation, in the vendored `third_party/` README).
- Whether the target is a discrete Radeon or the Ryzen iGPU; ADLX enumerates
  whichever is present, and the iGPU's control-input relevance is decided from
  the §12 evidence (its heat is already part of the CPU package).
- Any future NVIDIA+AMD dual-GPU fusion (explicitly out of scope for v1).

## Promotion

Promote this record to **Current** and check FEAT-0025 §13 gate 3 only after the
ADLX-vs-ADL choice and the logging-only-first ship order are confirmed by the
maintainer. FEAT-0025 stays **Draft** until then.
