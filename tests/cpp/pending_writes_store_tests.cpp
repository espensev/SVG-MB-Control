// Tests for FEAT-0019: PendingWritesStore::Upsert performs the synchronous
// atomic persist only when the recovery-relevant identity
// (channel, baseline_duty_raw, baseline_mode_raw) changes or the entry is new.
// A same-baseline target_pct change is deferred to the end-of-tick Flush()
// (marked dirty) rather than persisted synchronously before ApplyDuty.
//
// Persist is detected by file existence: each test removes the sidecar after a
// known persist, then asserts whether the next call recreated it.

#include "test_helpers.h"

#include "pending_writes.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace {

using svg_mb_control::PendingWriteEntry;
using svg_mb_control::PendingWritesStore;
using svg_mb_control::PendingWritesSidecarPath;
using svg_mb_control::ReadPendingWrites;

std::filesystem::path MakeTempHome(const char* name) {
    std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        (std::string("svg_mb_control_pending_writes_store_tests_") + name + "_" +
         UniqueTempSuffix());
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::create_directories(home, ec);
    return home;
}

bool SidecarExists(const std::filesystem::path& home) {
    std::error_code ec;
    return std::filesystem::exists(PendingWritesSidecarPath(home), ec);
}

void RemoveSidecar(const std::filesystem::path& home) {
    std::error_code ec;
    std::filesystem::remove(PendingWritesSidecarPath(home), ec);
}

PendingWriteEntry MakeEntry(std::uint32_t channel, std::uint8_t duty_raw,
                            std::uint8_t mode_raw, double target_pct) {
    PendingWriteEntry entry;
    entry.channel = channel;
    entry.baseline_duty_raw = duty_raw;
    entry.baseline_mode_raw = mode_raw;
    entry.target_pct = target_pct;
    entry.requested_hold_ms = 0u;
    entry.started_iso = "2026-06-18T00:00:00";
    return entry;
}

// REQ-WRITEHOT-01/04: first entry persists synchronously; a same-baseline target
// change does not persist synchronously but is written by the end-of-tick Flush().
void TestSameBaselineTargetChangeDefersToFlush() {
    const std::filesystem::path home = MakeTempHome("defer");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(2u, 10u, 1u, 30.0));
    ExpectTrue(SidecarExists(home),
               "first Upsert persists synchronously (activation)");

    RemoveSidecar(home);
    store.Upsert(MakeEntry(2u, 10u, 1u, 35.0));  // same baseline, new target
    ExpectFalse(SidecarExists(home),
                "same-baseline target change does not persist synchronously");

    store.Flush();
    ExpectTrue(SidecarExists(home),
               "end-of-tick Flush writes the deferred update");
    const auto entries = ReadPendingWrites(home);
    ExpectTrue(entries.size() == 1u && entries[0].target_pct == 35.0,
               "flushed sidecar carries the latest target_pct");
}

// REQ-WRITEHOT-01/04: a changed baseline persists synchronously and carries the
// latest in-memory target forward.
void TestBaselineChangePersistsSynchronously() {
    const std::filesystem::path home = MakeTempHome("baseline");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(2u, 10u, 1u, 30.0));
    store.Upsert(MakeEntry(2u, 10u, 1u, 35.0));  // same baseline, deferred (dirty)
    RemoveSidecar(home);
    store.Upsert(MakeEntry(2u, 20u, 1u, 40.0));  // baseline_duty_raw changed
    ExpectTrue(SidecarExists(home),
               "baseline re-capture persists synchronously");
    const auto entries = ReadPendingWrites(home);
    ExpectTrue(entries.size() == 1u && entries[0].baseline_duty_raw == 20u &&
                   entries[0].target_pct == 40.0,
               "persisted entry carries the new baseline and latest target");
}

// REQ-WRITEHOT-01/03: a new channel entry persists synchronously even when
// another channel already has a deferred (dirty) same-baseline update.
void TestNewChannelPersistsSynchronously() {
    const std::filesystem::path home = MakeTempHome("newchan");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(2u, 10u, 1u, 30.0));
    store.Upsert(MakeEntry(2u, 10u, 1u, 31.0));  // same baseline, deferred (dirty)
    RemoveSidecar(home);
    store.Upsert(MakeEntry(3u, 40u, 1u, 50.0));  // new channel
    ExpectTrue(SidecarExists(home),
               "first entry for a new channel persists synchronously");
    const auto entries = ReadPendingWrites(home);
    ExpectTrue(entries.size() == 2u, "both channels present after the persist");
}

// REQ-WRITEHOT-02: the recovery-relevant baseline is on disk after activation,
// regardless of later same-baseline target churn (no Flush required).
void TestRecoveryBaselineRecordedAtActivation() {
    const std::filesystem::path home = MakeTempHome("recover");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(4u, 12u, 1u, 24.0));   // activation persists
    store.Upsert(MakeEntry(4u, 12u, 1u, 28.0));   // deferred (dirty)
    store.Upsert(MakeEntry(4u, 12u, 1u, 33.0));   // deferred (dirty)

    const auto entries = ReadPendingWrites(home);  // read disk without Flush
    ExpectTrue(entries.size() == 1u, "one entry recorded for the channel");
    ExpectTrue(entries[0].channel == 4u && entries[0].baseline_duty_raw == 12u &&
                   entries[0].baseline_mode_raw == 1u,
               "recovery baseline (channel/duty/mode) is on disk from activation");
}

// REQ-WRITEHOT-04: the queued-removal + Flush path is unchanged.
void TestRemovalIsFlushed() {
    const std::filesystem::path home = MakeTempHome("removal");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(5u, 14u, 1u, 20.0));  // activation persists
    store.QueueRemove(5u);                        // marks dirty, no write yet
    store.Flush();                                // removal reaches disk
    const auto entries = ReadPendingWrites(home);
    ExpectTrue(entries.empty(), "QueueRemove + Flush removes the entry from disk");
}

// REQ-WRITEHOT-06 substrate: Flush() reports whether it performed the persist, so
// the tick loop clears persist-failure counters only after an actual full-file
// flush (and never on a no-op flush).
void TestFlushReportsWhetherItPersisted() {
    const std::filesystem::path home = MakeTempHome("flushreport");
    PendingWritesStore store(home);

    store.Upsert(MakeEntry(6u, 16u, 1u, 22.0));  // activation persists, clears dirty
    ExpectFalse(store.Flush(), "Flush with nothing pending returns false");

    store.Upsert(MakeEntry(6u, 16u, 1u, 27.0));  // same baseline -> deferred (dirty)
    ExpectTrue(store.Flush(),
               "Flush of a deferred change returns true (it persisted)");
    ExpectFalse(store.Flush(),
                "a second Flush with nothing pending returns false");
}

}  // namespace

int main() {
    TestSameBaselineTargetChangeDefersToFlush();
    TestBaselineChangePersistsSynchronously();
    TestNewChannelPersistsSynchronously();
    TestRecoveryBaselineRecordedAtActivation();
    TestRemovalIsFlushed();
    TestFlushReportsWhetherItPersisted();
    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "pending_writes_store_tests passed\n";
    return 0;
}
