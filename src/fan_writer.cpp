// Public fan-writer factory. The two strategy implementations live in
// simulated_fan_writer.cpp and sio_fan_writer.cpp; shared result-factory
// helpers are header-inline in fan_writer_internal.h.

#include "fan_writer.h"

#include "env_util.h"
#include "fan_writer_internal.h"

#include <memory>
#include <string>
#include <string_view>

namespace svg_mb_control {

std::unique_ptr<FanWriter> CreateFanWriter(
    const RuntimeWritePolicy& runtime_policy) {
    const std::string sim_mode = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE", "");
    if (sim_mode == "enabled") {
        return MakeSimulatedFanWriter(runtime_policy);
    }

    return MakeSioFanWriter(runtime_policy);
}

}  // namespace svg_mb_control
