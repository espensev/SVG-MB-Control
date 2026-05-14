# Discovery - GPU Temp Envelope

> Historical note, 2026-05-14: this discovery explains why the GPU envelope
> became the primary airflow signal. The current packaged config also includes
> per-channel `cpu_override_curve` overlays, so statements about GPU-only
> channels ignoring CPU-only spikes are no longer the full live behavior.

**Goal:** Check `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl` for how GPU temperatures are read and apply the relevant behavior before more `svg-mb-control` testing.
**Date:** 2026-05-12
**Status:** complete
**Recommended next:** run a GPU load test and evaluate whether the low-floor
GPU-only case channels need a slower or steeper ramp.

---

## Questions

1. How does `NVG_SmoothControl` read GPU temperatures?
2. Which GPU temperature does `NVG_SmoothControl` treat as the control envelope?
3. What does `svg-mb-control` currently use for GPU control input?
4. What is the smallest safe correction before further case-fan testing?

---

## Findings

### Q1: How does `NVG_SmoothControl` read GPU temperatures?

**Answer:** It uses the vendored `NvApiController` path. A single `read_thermals()` call returns core, hotspot, memory junction, and raw values.

**Evidence:**
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\include\nvapi_controller.h:58` - documents that one `read_thermals()` call returns core, hotspot, memory junction, and raw values.
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\src\gpu_api.cpp:24` - calls `ctrl_.read_thermals(gpu_, thermals_)`.

**Implications:**
- `svg-mb-control` should keep using the existing vendored telemetry reader rather than adding a subprocess or sibling-repo dependency.

### Q2: Which GPU temperature does `NVG_SmoothControl` treat as the control envelope?

**Answer:** Core is the fast signal, memory junction is the slow thermal envelope, and hotspot is promoted into the envelope when valid.

**Evidence:**
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\include\nvapi_controller.h:27` - core is the fast control signal.
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\include\nvapi_controller.h:35` - memory junction is the slow envelope.
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\src\main.cpp:1182` - detects when hotspot should become primary.
- `D:\Development\Thermals\TempControl-Nvidia\NVG_SmoothControl\src\main.cpp:1183` - promotes hotspot by taking `max(memj, hotspot)`.

**Implications:**
- Case-fan control should respond to GPU memory junction and hotspot, not only core.

### Q3: What does `svg-mb-control` currently use for GPU control input?

**Answer:** Before this change, it already read GPU core, memory junction, and hotspot, but the control input used only `max(core, memjn)`.

**Evidence:**
- `src/gpu_reader.cpp:52` - samples `GpuSampleMode::ThermalFast`.
- `src/gpu_reader.cpp:57` - copies `core_c`.
- `src/gpu_reader.cpp:58` - copies `memjn_c`.
- `src/gpu_reader.cpp:59` - copies `hotspot_c`.
- `src/control_loop.cpp:640` before this change used `max(core_c, memjn_c)`.

**Implications:**
- The repo was already reacting to GPU memory junction on `max_cpu_gpu` channels, but hotspot was ignored in the control blend.

### Q4: What is the smallest safe correction before further case-fan testing?

**Answer:** Use a GPU control envelope of `max(core, memjn, hotspot-if-valid)`, move the packaged fan curve breakpoints toward GPU memory-envelope temperatures, and make the low-floor case channels GPU-driven.

**Evidence:**
- `src/control_loop.cpp:434` - added `GpuControlEnvelopeC`.
- `src/control_loop.cpp:648` - `TempInputs.gpu_c` now uses that envelope.
- `config/control.release.json:25` - packaged curve now starts at a 50 C envelope point instead of 60 C.
- `config/control.release.json:21` - channel `0` uses `gpu_only`.
- `config/control.release.json:52` - channel `1` uses `gpu_only` for the higher-floor front radiator intake path.
- `config/control.release.json:88` - channel `2` uses `gpu_only` for the higher-floor front intake response.
- `config/control.release.json:128` - channel `3` uses `gpu_only` for the higher-floor front intake response.
- `config/control.release.json:168` - channel `4` uses `gpu_only`.
- `config/control.release.json:202` - channel `5` uses `gpu_only`.

**Implications:**
- The next run will react earlier to GPU memory heat while still keeping low floors and avoiding fast/noisy fan response.

---

## Cross-Cutting Analysis

### Constraints

- RTX 5090 may report no hotspot sensor; hotspot is `0.0` when absent, so the control envelope must ignore zero hotspot values.
- Memory junction updates slowly, so it should shape steady airflow rather than trigger twitchy write behavior.
- Future JSON changes still require a controller restart, and future source changes still require a release rebuild.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Running process continues old curve | Low | Medium | Config is read at startup; the 2026-05-12 restart loaded the GPU-only split. |
| Release binary does not include hotspot promotion yet | Low | Low | The release build completed after the source change. |
| GPU memory curve under-reacts to sustained load | Medium | Medium | Current settings are intentionally low/noise-first; evaluate under GPU load before raising floors. |
| GPU envelope without CPU overlay ignores CPU-only benchmark spikes | Medium | Low | Historical risk only; the current packaged config adds per-channel CPU overlays. |

### Open Questions

- Exact physical mapping of channels `1,4,5` to front/center/rear radiator lanes remains inferred, not proven by source code.

---

## Recommendation

Proceed with live testing on the restarted controller. Watch GPU memory junction,
fan RPM, and `control_runtime.json` channel setpoints during a sustained GPU
load before raising floors.
