#pragma once

#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#include <limits>

namespace svg_mb_control {

class AmdReader;
class FanWriter;
class GpuReader;

struct DirectRuntimeSnapshotTiming {
    double total_read_ms = std::numeric_limits<double>::quiet_NaN();
    double amd_read_ms = std::numeric_limits<double>::quiet_NaN();
    double gpu_thermal_read_ms = std::numeric_limits<double>::quiet_NaN();
    double fan_scan_read_ms = std::numeric_limits<double>::quiet_NaN();
};

RuntimeSnapshot SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy);

// Fill-in-place variant for the steady-state control loop: reuses `out`'s
// heap buffers across ticks instead of constructing a fresh snapshot every
// call. `out` is fully reset (telemetry vectors cleared, scalars overwritten)
// so no value carries over from a previous tick.
void SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy,
    RuntimeSnapshot& out);

void SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy,
    RuntimeSnapshot& out,
    DirectRuntimeSnapshotTiming* timing);

bool RuntimeSnapshotHasTelemetry(const RuntimeSnapshot& snapshot);

}  // namespace svg_mb_control
