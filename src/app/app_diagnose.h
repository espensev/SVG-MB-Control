#pragma once

#include <string>

namespace svg_mb_control {

struct ControlConfig;

// `--diagnose-amd`: prints the AMD reader's init state plus one sample
// (CPU label, transport path, last warning, per-sensor temperatures).
// Returns 0 when the sample is available, 1 otherwise.
int RunDiagnoseAmd();

// `--diagnose-gpu`: prints the GPU reader's init state plus one sample
// (gpu name, core/memjn/hotspot, last warning). Returns 0 when the
// sample is available, 1 otherwise.
int RunDiagnoseGpu();

// Default-mode (one-shot) snapshot: runs one direct AMD/GPU/fan sample
// pass and returns its JSON representation. Used by RunApp's
// fall-through path when no long-running mode is selected.
std::string SampleDirectSnapshotJson(const ControlConfig* config);

}  // namespace svg_mb_control
