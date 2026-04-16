#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svg_mb_control {

struct PendingWriteEntry {
    std::uint32_t channel = 0u;
    std::uint8_t baseline_duty_raw = 0u;
    std::uint8_t baseline_mode_raw = 0u;
    double target_pct = 0.0;
    std::uint32_t requested_hold_ms = 0u;
    std::string started_iso;
    std::uint32_t child_pid = 0u;
};

// Returns the entries currently recorded in the pending-writes sidecar.
// Returns an empty vector if the file does not exist. Throws
// std::runtime_error if the file exists but cannot be parsed.
std::vector<PendingWriteEntry> ReadPendingWrites(
    const std::filesystem::path& runtime_home);

// Rewrites the sidecar file to contain the given entries. Write is atomic
// via temp-file + std::filesystem::rename. Throws on filesystem failure.
void WritePendingWrites(const std::filesystem::path& runtime_home,
                        const std::vector<PendingWriteEntry>& entries);

// Appends a new entry to the sidecar, preserving existing entries. If an
// entry for the same channel already exists, it is replaced.
void UpsertPendingWrite(const std::filesystem::path& runtime_home,
                        const PendingWriteEntry& entry);

// Removes the entry for the given channel. No-op if no matching entry.
void RemovePendingWrite(const std::filesystem::path& runtime_home,
                        std::uint32_t channel);

std::filesystem::path PendingWritesSidecarPath(
    const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
