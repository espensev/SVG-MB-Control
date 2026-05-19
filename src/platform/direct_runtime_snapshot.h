#pragma once

#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

namespace svg_mb_control {

class AmdReader;
class FanWriter;
class GpuReader;

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

bool RuntimeSnapshotHasTelemetry(const RuntimeSnapshot& snapshot);

}  // namespace svg_mb_control
