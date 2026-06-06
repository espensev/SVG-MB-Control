#pragma once

#include "control_config.h"
#include "runtime_write_policy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace svg_mb_control {

struct WriteRequest {
    std::uint32_t channel = 0u;
    double target_pct = 0.0;
    std::uint32_t hold_ms = 0u;
};

struct BaselineCapture {
    bool present = false;                  // true when target channel found in snapshot
    bool snapshot_channel_present = false; // true when snapshot carried per-channel policy metadata
    std::uint32_t channel = 0u;
    std::uint8_t duty_raw = 0u;
    std::uint8_t mode_raw = 0u;
    bool write_allowed = false;
    bool policy_blocked = false;
    bool effective_write_allowed = false;
    std::string snapshot_time_iso;
    bool freshness_ok = false;
    std::int64_t snapshot_age_ms = -1;
    bool policy_writes_enabled_present = false;
    bool policy_writes_enabled = false;
};

// Parses a runtime snapshot JSON and extracts the baseline
// state for the given channel. Also validates snapshot freshness against
// the provided `now` wall clock using snapshot_time parsed as local time.
BaselineCapture CaptureBaselineFromSnapshotJson(
    const std::string& snapshot_json,
    std::uint32_t channel,
    std::uint32_t freshness_ceiling_ms,
    std::chrono::system_clock::time_point now);

// Runs one bounded write-once session:
//   snapshot -> baseline check -> sidecar upsert -> direct duty write
//   -> hold -> direct restore -> sidecar remove on clean exit.
// Returns 0 on clean exit, non-zero on failure (including policy refusal
// and restore failure).
int RunWriteOnce(const ControlConfig& config,
                 const std::filesystem::path& runtime_home,
                 const WriteRequest& request,
                 const std::atomic<bool>& stop_flag);

// Reads the pending-writes sidecar and restores each entry directly
// through the Control-owned writer. Entries are removed after a
// successful restore. Returns 0 when all entries were restored (or
// there were none). Returns non-zero when any restore fails; remaining
// entries stay in the sidecar.
int ReconcilePendingWrites(const std::filesystem::path& runtime_home,
                           const RuntimeWritePolicy& runtime_policy,
                           std::uint32_t restore_timeout_ms);

}  // namespace svg_mb_control
