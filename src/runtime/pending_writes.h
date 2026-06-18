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

// The path a corrupt sidecar is quarantined to (FEAT-0012):
// `<runtime_home>/pending_writes.json.corrupt`.
std::filesystem::path QuarantinedSidecarPath(
    const std::filesystem::path& runtime_home);

// Result of a fault-tolerant sidecar read (FEAT-0012).
struct TolerantPendingWritesRead {
    std::vector<PendingWriteEntry> entries;
    bool quarantined = false;  // true if a corrupt file was renamed aside
    std::string detail;        // diagnostic for the quarantine event
};

// Reads the pending-writes sidecar; on a parse/shape failure it quarantines the
// corrupt file (renames it to QuarantinedSidecarPath) and returns no entries
// with quarantined=true, so a corrupt sidecar at startup is not fatal
// (FEAT-0012 REQ-SIDECARRESIL-01/02). A missing file returns no entries with
// quarantined=false. Does not throw on a corrupt file.
TolerantPendingWritesRead ReadPendingWritesTolerant(
    const std::filesystem::path& runtime_home);

// In-memory cache for the pending-writes sidecar. Avoids re-reading and
// re-parsing the JSON file on every Upsert/Remove call. Upsert persists
// synchronously only when a channel's captured baseline changes (first
// activation or re-capture); same-baseline target churn is deferred to Flush()
// (FEAT-0019), so the crash-recovery contract (sidecar reflects every active
// channel's captured baseline before ApplyDuty) is preserved while no fsync'd
// file-replace runs before ApplyDuty during a ramp. Remove is queued; callers
// must invoke Flush() at a safe point (e.g. end of tick).
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
    // Returns true iff this call performed the synchronous persist (so the
    // caller can clear its persist-failure health signal only on an actual
    // write — FEAT-0019 REQ-WRITEHOT-06). A deferred same-baseline change
    // returns false; a persist that throws propagates the exception.
    virtual bool Upsert(const PendingWriteEntry& entry) = 0;
    virtual void QueueRemove(std::uint32_t channel) = 0;
};

class PendingWritesStore : public PendingWritesStoreInterface {
 public:
    explicit PendingWritesStore(std::filesystem::path runtime_home);

    // Loads existing sidecar contents from disk into the in-memory cache.
    // Throws on parse failure; missing file is treated as empty.
    void Load();

    // Replaces the in-memory cache with already-read entries without re-reading
    // the file (lets reconcile parse the sidecar once; FEAT-0012).
    void Adopt(std::vector<PendingWriteEntry> entries);

    // Inserts or replaces the entry for entry.channel. Persists the sidecar to
    // disk synchronously ONLY when the recovery-relevant identity changes — a
    // new channel entry (first activation) or a changed baseline_duty_raw /
    // baseline_mode_raw — so the crash-recovery record (sidecar reflects every
    // active channel's captured baseline before ApplyDuty) is preserved. A
    // same-baseline target_pct/hold/started_iso change marks the store dirty for
    // the next Flush() instead of writing synchronously (FEAT-0019), because
    // those fields are recovery-irrelevant. Returns true iff it persisted
    // synchronously (false for a deferred change), so the caller clears its
    // persist-failure health signal only on an actual write (REQ-WRITEHOT-06).
    // Throws on filesystem failure when it does persist.
    bool Upsert(const PendingWriteEntry& entry) override;

    // Marks the entry for the given channel as removed. Does not touch
    // disk until Flush() is called.
    void QueueRemove(std::uint32_t channel) override;

    // Persists the in-memory state to disk if it has changed since the last
    // write (queued removals and FEAT-0019 deferred same-baseline Upserts).
    // Returns true iff it performed the synchronous persist; false if there was
    // nothing to flush. A successful flush rewrites the whole sidecar, so every
    // channel's record is then current (the caller uses this to clear stale
    // persist-failure health signals — REQ-WRITEHOT-06). Throws on filesystem
    // failure when it does persist.
    bool Flush();

 private:
    void Persist();

    std::filesystem::path runtime_home_;
    std::vector<PendingWriteEntry> entries_;
    bool loaded_ = false;
    bool dirty_ = false;
};

}  // namespace svg_mb_control
