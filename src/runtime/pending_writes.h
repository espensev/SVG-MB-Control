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

// In-memory cache for the pending-writes sidecar. Avoids re-reading and
// re-parsing the JSON file on every Upsert/Remove call. Upsert still
// persists synchronously so the crash-recovery contract (sidecar reflects
// any in-flight fan write before ApplyDuty runs) is preserved. Remove is
// queued; callers must invoke Flush() at a safe point (e.g. end of tick).
//
// Not thread-safe: intended for single-threaded use inside the control loop.

// Abstract seam over the pending-writes store so the control write path
// (TryApplyChannelSetpoint) can be exercised with an injected store that throws
// on persist (FEAT-0010). PendingWritesStore is the production implementation;
// only the methods the write path calls through the injected reference (Upsert,
// QueueRemove) are virtual.
class PendingWritesStoreInterface {
 public:
    virtual ~PendingWritesStoreInterface() = default;
    virtual void Upsert(const PendingWriteEntry& entry) = 0;
    virtual void QueueRemove(std::uint32_t channel) = 0;
};

class PendingWritesStore : public PendingWritesStoreInterface {
 public:
    explicit PendingWritesStore(std::filesystem::path runtime_home);

    // Loads existing sidecar contents from disk into the in-memory cache.
    // Throws on parse failure; missing file is treated as empty.
    void Load();

    // Inserts or replaces the entry for entry.channel and persists the
    // sidecar to disk synchronously. Throws on filesystem failure.
    void Upsert(const PendingWriteEntry& entry) override;

    // Marks the entry for the given channel as removed. Does not touch
    // disk until Flush() is called.
    void QueueRemove(std::uint32_t channel) override;

    // Persists any queued removals to disk if the in-memory state has
    // changed since the last write. No-op if there is nothing to flush.
    void Flush();

 private:
    void Persist();

    std::filesystem::path runtime_home_;
    std::vector<PendingWriteEntry> entries_;
    bool loaded_ = false;
    bool dirty_ = false;
};

}  // namespace svg_mb_control
