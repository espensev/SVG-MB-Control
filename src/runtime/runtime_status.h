#pragma once

#include "control_runtime_context.h"
#include "hardware_access_status.h"
#include "read_loop.h"
#include "runtime_csv_rows.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control {

// Subset of a controlled-channel JSON entry used by the runtime/health
// consumers. Keep narrow on purpose: only the fields needed for activity
// and degraded-channel checks. Adding fields here is cheaper than adding
// raw JsonStringOr / JsonBoolOr calls back into the consumers.
struct RuntimeStatusChannelSnapshot {
    std::uint32_t channel = 0u;
    bool circuit_breaker_open = false;
    bool sensor_failed = false;
    std::uint32_t consecutive_write_failures = 0u;
    // FEAT-0010 (REQ-WRITESAFE-03): sidecar-persist failures degrade health.
    std::uint32_t consecutive_sidecar_persist_failures = 0u;
};

// Typed view of <runtime_home>/control_runtime.json that both schemas
// (svg_mb_control.runtime_status.control_loop.v4 and
// svg_mb_control.runtime_status.read_loop.v1) collapse into. Fields not
// present in the source schema default to empty / 0.
struct RuntimeStatusSnapshot {
    std::string mode;
    std::string status;
    std::string status_detail;
    std::uint32_t process_id = 0u;
    // last_update_iso is the latest evaluation timestamp from the
    // control-loop schema (loop_last_evaluation) OR the latest refresh
    // timestamp from the read-loop schema (last_refresh). The reader
    // resolves the schema difference so consumers do not have to.
    std::string last_update_iso;
    std::string last_successful_restore_iso;
    std::string log_csv_path;
    std::string event_log_path;
    HardwareAccessStatus hardware_access;
    bool stale = false;
    std::vector<RuntimeStatusChannelSnapshot> controlled_channels;

    // True when process_id refers to a live process AND the published
    // status is not a terminal "shutdown" / "failed" /
    // "direct-read-failed". Used by the launcher's duplicate-detection
    // and stop-wait paths.
    bool LooksActive() const;

    // Sum of channels that report a circuit breaker open, sensor failure, any
    // consecutive write failures, or any sidecar-persistence failures.
    std::uint32_t DegradedChannelCount() const;
};

// Reads <runtime_home>/control_runtime.json. Returns nullopt when the file
// is absent or not a JSON object. Tolerates unknown / extra fields. Does
// not throw — JSON parse errors surface as nullopt.
std::optional<RuntimeStatusSnapshot> ReadRuntimeStatus(
    const std::filesystem::path& runtime_home);

// Writes the control-loop schema (v4). Moved here from
// control_status_writer.cpp so the dual-schema file has one owner.
bool WriteControlLoopStatus(const std::filesystem::path& runtime_home,
                            const std::string& mode_label,
                            const std::string& status,
                            const std::string& status_detail,
                            std::uint64_t tick_count,
                            const std::string& last_evaluation_iso,
                            const RuntimeControlLoopTimingState& timing,
                            const std::vector<ChannelState>& channels,
                            const std::string& log_csv_path,
                            const std::string& log_manifest_path,
                            const std::string& event_log_path,
                            const std::string& last_successful_restore_iso = {},
                            // FEAT-0023 (REQ-MPROFILE-09): additive,
                            // observational active-profile identity in the
                            // worker runtime status (name + resolution source).
                            const std::string& active_profile_name = {},
                            const std::string& active_profile_source = {},
                            const HardwareAccessStatus& hardware_access =
                                HardwareAccessStatus{});

// Writes the read-loop schema (v1). Moved here from a file-local helper
// inside read_loop.cpp.
bool WriteReadLoopStatus(const std::filesystem::path& runtime_home,
                         const ReadLoop::Status& status);

}  // namespace svg_mb_control
