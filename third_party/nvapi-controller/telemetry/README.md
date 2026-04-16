# gpu_telemetry

Reusable NVIDIA GPU telemetry library for Windows. It wraps NVAPI and NVML behind
small APIs centered on `GpuProbe`, `GpuSensorReader`, `GpuSnapshot`, and a
plain C ABI for DLL/FFI callers.

> **Vendored note:** In `SVG-MB-Control`, this library is built from the
> repo-local `third_party/nvapi-controller/telemetry` tree through the
> top-level CMake project. The standalone configure and install instructions
> below work directly against this vendored copy.

## Public API

Installed consumers should treat only these headers as stable:

- `gpu_telemetry/gpu_telemetry_c.h`
- `gpu_telemetry/gpu_sensor_reader.h`
- `gpu_telemetry/gpu_probe.h`
- `gpu_telemetry/gpu_snapshot.h`

`GpuProbe` exposes shareable discovery metadata through `GpuInfo` and keeps
vendor-specific handles and loader state private to the implementation.
`GpuSensorReader` adds a simpler app-facing facade for one-shot GPU sampling.
`gpu_telemetry_c.h` exposes a stable C ABI with POD structs so the DLL can be
loaded from Python, C, Rust FFI, or other non-C++ callers.

Other headers in this repository support the implementation or low-level
research work, but they are not part of the installed package contract and are
not installed by `cmake --install`.

## Quick Sensor Reader Example

```cpp
#include <gpu_telemetry/gpu_sensor_reader.h>

#include <cstdio>
#include <string>

int main() {
    GpuSensorReader reader;
    std::string warning;
    if (!reader.init(warning)) {
        std::fprintf(stderr, "gpu init failed: %s\n", warning.c_str());
        return 1;
    }

    auto gpus = reader.gpu_info();
    auto samples = reader.sample_all(GpuSampleMode::Full);

    for (size_t i = 0; i < samples.size(); ++i) {
        std::printf("GPU %d %s core=%.1fC fan=%urpm power=%.1fW\n",
                    samples[i].gpu_index,
                    gpus[i].name.c_str(),
                    samples[i].core_c,
                    samples[i].fan_count > 0 ? samples[i].fans[0].rpm : 0u,
                    samples[i].nvml_power_mw / 1000.0);
    }

    reader.shutdown();
    return 0;
}
```

`GpuSampleMode::Full` combines the existing fast and slow probe paths into one
`GpuSnapshot`, which is the most useful mode for a motherboard or system-sensor
application. The other modes expose the lower-latency probe tiers directly when
you want tighter polling or lower overhead.

## Polling Model

This library does not start a background thread or own a polling cadence. The
caller decides when to call `sample()` or `sample_all()` and how to schedule
fast vs. slow tiers.

Each `GpuProbe` sample method returns `bool`: `true` when a sample was read
from the driver, `false` only for invalid `gpu_index`. The library does not
enforce any minimum polling interval — the caller owns the poll cadence and
can sample as fast as the hardware supports (down to 1ms or below).

> **Design rationale:** This library is a thin driver interface, not a policy
> layer. Different consumers have wildly different needs — a fan-control loop
> polling at 1 Hz, a burst-capture tool at 1 kHz, a research bench pushing
> hardware limits. The library provides the capability; the consumer decides
> what makes sense for their hardware, workload, and power budget. If the
> driver returns stale data at very high poll rates, that is visible in the
> timestamps and is the caller's concern, not the library's.

Available sample tiers, from cheapest to heaviest:

| Tier | Method | Typical cost | Data |
|------|--------|-------------|------|
| Thermal fast | `sample_thermal_fast` | <1 ms | Undocumented core/memjn temp (Q8.8) |
| Fast | `sample_fast` | ~1 ms | Thermals, clocks, utilization, pstate |
| Medium | `sample_medium` | ~1-2 ms | Clocks, utilization, power, NVML temp |
| Slow | `sample_slow` | ~5-10 ms | Fans, power, VRAM, PCIe, throttle, voltage |
| Rare | `sample_rare` | ~5-10 ms | VRAM, PCIe, throttle, voltage, encoder/decoder |

### Empirical sensor refresh rates (RTX 5090, Blackwell)

Measured under CUDA nbody load (1M bodies, full GPU utilization):

| Tier | Configured | Achieved | Notes |
|------|-----------|----------|-------|
| Thermal fast | 5 ms | ~5-6 ms (148 Hz) | Consistent 5-6ms inter-sample delta in burst dumps |
| Fast | 16 ms | not yet characterized | Includes undoc thermals + documented clocks/util |
| Medium | 100 ms | not yet characterized | Adds NVML temp, power |
| Slow | 500 ms | not yet characterized | Fans, VRAM, PCIe — heavier driver calls |
| Rare | 500 ms | not yet characterized | Encoder/decoder, voltage — heaviest |

**Key observations:**
- The undocumented thermal sensor (Q8.8 fixed-point) refreshes at hardware
  level every ~5 ms on RTX 5090. Polling faster returns duplicate readings.
- Under heavy CUDA load, scheduling jitter adds ~1-2 ms, so actual throughput
  at 5ms configured is ~148 samples/s rather than the theoretical 200.
- These rates are hardware- and generation-dependent. RTX 3080 (Ampere) may
  differ. Testing on each target GPU is recommended.

`GpuSensorReader::sample()` clears the output snapshot before each attempt.
`sample_all()` returns one snapshot per GPU, each starting zeroed.

`GpuSnapshot::time_ms` and `GpuSnapshot::dt_ms` are caller-owned metadata.
The library fills the sensor fields in each snapshot, while the caller can
stamp the sample with its own timing origin and delta if those values are
useful to the application.

## Build

```powershell
cmake -S third_party/nvapi-controller/telemetry -B build/gpu_telemetry -G Ninja
cmake --build build/gpu_telemetry --config Release
```

The library dynamically loads `nvapi64.dll` and `nvml.dll` at runtime. It does
not require the NVIDIA SDK at build time. The release build emits:

- `dist/x64/lib/gpu_telemetry.lib`: C++ static library
- `bin/x64/gpu_telemetry_c.dll`: C ABI DLL for FFI callers
- `dist/x64/lib/gpu_telemetry_c.lib`: import library for the C ABI DLL

The packaged release also bundles the required MSVC runtime DLLs under `bin/`.
The exact runtime dependency list and hashes are recorded in
`build-info.json` and `PACKAGE_CONTENTS.json`.

## Release Packaging

Inside `SVG-MB-Control`, this vendored slice is normally packaged as part of the
top-level release flow. For a standalone install tree, use the direct CMake
install flow below.

## Install As CMake Package

```powershell
cmake -S third_party/nvapi-controller/telemetry -B build/gpu_telemetry -G Ninja
cmake --build build/gpu_telemetry --config Release
cmake --install build/gpu_telemetry --prefix .\out\gpu_telemetry
```

Downstream projects can then consume it with:

```cmake
find_package(gpu_telemetry CONFIG REQUIRED)
target_link_libraries(my_cpp_app PRIVATE gpu_telemetry::gpu_telemetry)
target_link_libraries(my_c_or_ffi_host PRIVATE gpu_telemetry::gpu_telemetry_c)
```

For `gpu_telemetry::gpu_telemetry_c`, ship `bin/gpu_telemetry_c.dll` alongside
your executable or ensure the package `bin/` directory is on `PATH`.

## C ABI / Python FFI

Use `gpu_telemetry/gpu_telemetry_c.h` together with `gpu_telemetry_c.dll` when
you need a stable ABI instead of the C++ classes.

The C ABI exposes:

- `gpu_telemetry_get_api_version` -- runtime version check (compare against
  `GPU_TELEMETRY_CAPI_VERSION` to detect header/DLL mismatches)
- an opaque `gpu_telemetry_reader_t` handle
- `gpu_telemetry_reader_create/destroy/init/shutdown`
- `gpu_telemetry_reader_get_gpu_count`
- `gpu_telemetry_reader_get_gpu_info`
- `gpu_telemetry_reader_sample`

`gpu_telemetry_reader_sample()` returns `1` only when it produced a fresh
sample. It returns `0` for invalid inputs, uninitialized readers, or
unsupported sample modes.

Minimal Python `ctypes` usage:

```python
import ctypes as ct

gpu = ct.WinDLL("gpu_telemetry_c.dll")
reader = gpu.gpu_telemetry_reader_create()
```

## Notes

- `GpuSnapshot` uses `0`, `false`, or `-1` sentinel values when a metric is
  unavailable.
- NVML-backed fields remain optional at runtime. A probe can still initialize
  when `nvml.dll` is missing.
- Internal headers in this repository support the implementation, but they are
  not part of the installed package contract.
