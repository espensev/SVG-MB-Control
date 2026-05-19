#pragma once

#include "gpu_reader.h"

#include <sstream>
#include <string>

namespace svg_mb_control {

// CSV column names for the GPU evidence block, leading with a comma so the
// block can be appended directly to a common header.
std::string BuildGpuEvidenceCsvHeader();

// Appends one GPU evidence row segment (leading comma per field) matching the
// column order produced by BuildGpuEvidenceCsvHeader.
void AppendGpuEvidenceCsvRow(std::ostringstream& csv,
                             const GpuEvidenceSample& sample);

}  // namespace svg_mb_control
