#include "runtime_lifecycle.h"

#include "control_scheduler.h"
#include "json_io.h"

#include <chrono>
#include <ctime>
#include <string>
#include <system_error>

namespace svg_mb_control {

std::filesystem::path RuntimeStopRequestPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "stop.request.json";
}

bool RuntimeStopRequested(const std::filesystem::path& runtime_home) {
    std::error_code ec;
    return std::filesystem::exists(RuntimeStopRequestPath(runtime_home), ec) &&
           !ec;
}

bool RequestRuntimeStop(const std::filesystem::path& runtime_home) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["requested_at"] =
        FormatLocalIso8601(std::chrono::system_clock::now());
    payload["reason"] = "operator-request";
    return TryWriteJsonFileAtomic(RuntimeStopRequestPath(runtime_home),
                                  payload);
}

void ClearRuntimeStopRequest(const std::filesystem::path& runtime_home) {
    std::error_code ec;
    std::filesystem::remove(RuntimeStopRequestPath(runtime_home), ec);
}

}  // namespace svg_mb_control
