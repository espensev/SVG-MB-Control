#pragma once

// Read-only Windows-service feasibility probe (Runtime Robustness Step 5 of
// the Response Lane Split campaign). Verifies the capabilities a Session 0 /
// LocalSystem service would need before the runtime is ever moved off the
// scheduled-task launcher:
//
//   - AMD CPU telemetry can be read              (required)
//   - GPU telemetry can be read                  (advisory; GPU is optional)
//   - SIO fan state can be read                  (required)
//   - the runtime home is writable               (required)
//   - a no-write SIO open/close leaves no pending-write sidecar (required)
//
// The probe performs NO fan-duty writes. It only constructs the read path,
// reads, and tears it down. Returns 0 when all required checks pass, 1 when
// one or more required checks fail.

#include <filesystem>

namespace svg_mb_control {

struct ControlConfig;

int RunServiceProbe(const std::filesystem::path& runtime_home,
                    const ControlConfig* config,
                    bool json_output);

}  // namespace svg_mb_control
