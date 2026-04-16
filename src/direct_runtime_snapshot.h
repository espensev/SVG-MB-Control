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

bool RuntimeSnapshotHasTelemetry(const RuntimeSnapshot& snapshot);

}  // namespace svg_mb_control
