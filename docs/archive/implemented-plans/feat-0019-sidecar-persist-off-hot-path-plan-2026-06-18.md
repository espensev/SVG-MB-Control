# FEAT-0019 Sidecar persistence off the actuation hot path — Implementation Plan

**Archive status:** implemented and archived 2026-06-20. This file is retained
for audit history only; do not execute it as a current plan. Current status lives
in `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md`,
`docs/TRACEABILITY.md`, and `docs/features/README.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the synchronous `pending_writes.json` persist off the actuation hot
path: `PendingWritesStore::Upsert` persists synchronously only when a channel's
recovery-relevant identity `(channel, baseline_duty_raw, baseline_mode_raw)` changes
or its entry is new, and defers same-baseline churn to the existing once-per-tick
end-of-tick `Flush()`, so no fsync'd file-replace runs before `ApplyDuty` during a
ramp.

**Architecture:** Task 1 is a small behavioral change inside one method
(`PendingWritesStore::Upsert`, `src/runtime/pending_writes.cpp`): detect whether the
upsert changes the recovery-relevant identity; if not, mark the store dirty and let
the existing end-of-tick `Flush()` write it instead of persisting synchronously.
Recovery (`ReconcilePendingWrites`) and health (`runtime_health.cpp`) are verified to
read only the baseline / readability, never `target_pct`, so this is
behavior-preserving for crash recovery. Task 1b then corrects one cross-feature
interaction: because a deferred `Upsert` returns normally, the FEAT-0010
persist-failure counter reset at the `TryApplyChannelSetpoint` call site
(`src/control/channel_write.cpp`) must be made conditional on an actual persist
(REQ-WRITEHOT-06, mechanism per FEAT-0019 §11).

**Tech Stack:** C++17, CMake/CTest, the bespoke `tests/cpp/test_helpers.h`
assertion harness (`ExpectTrue`/`ExpectEqual`, `g_failures`, per-file `main()`),
`nlohmann::json` via `WriteJsonFileAtomic`.

## Global Constraints

- Spec: `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md`
  (`REQ-WRITEHOT-01..05`). The decision record
  `docs/control-latency-reduction-design-2026-06-18.md` (D-WRITEHOT-1) must be
  promoted from `Proposed` to `Current` before this is implementation-authorized
  (FEAT-0019 §13 gate 3) — see Task 0.
- No schema change: `pending_writes.json` keeps `schema_version = 1` and all existing
  fields (`src/runtime/pending_writes.cpp` `PendingWriteEntryToJson`). Existing
  sidecars and crash recovery stay valid (`AGENTS.md` §Repo Boundary, `RUNTIME_HOME.md`).
- No measurement-gate crossing: cadence, channels, and mixed-input strategy are
  untouched; this strictly reduces synchronous writes (`docs/MEASUREMENT_GATE.md`).
- Preserve the FEAT-0010 invariant: a persist that does run and throws must still
  fall through to `ApplyDuty` — do not add any new throw before actuation
  (`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`).
- Build/validate with `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (no publish, no live
  runtime interaction — `AGENTS.md` §Live Runtime Safety).
- Doctrine (`CLAUDE.md`): grounded claims, correct `must`/`should`/`is`, no undefined
  terms in the doc updates.

---

### Task 0: Promote the decision record (gate 3) — precondition, not code

**Files:**
- Modify: `docs/control-latency-reduction-design-2026-06-18.md` (D-WRITEHOT-1 status)
- Modify: `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md:9` (§9 decision row → Current; §13 gate 3 → `[x]`)

This is a maintainer authorization step. Do not start Task 1 until the maintainer
confirms D-WRITEHOT-1 is accepted.

- [ ] **Step 1: Flip D-WRITEHOT-1 to Current**

In `docs/control-latency-reduction-design-2026-06-18.md`, change the D-WRITEHOT-1
status line from `**Status: Proposed.**` to `**Status: Current (accepted 2026-MM-DD).**`
(use the acceptance date).

- [ ] **Step 2: Mark FEAT-0019 §9 row Current and gate 3 checked**

In `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md`, §9 decision-row Status
cell `Proposed (...)` → `Current`; §13 item 3 `- [ ] 3.` → `- [x] 3.`. Leave the
**Status** header at `Draft` until Task 3 flips it to `Implemented`.

- [ ] **Step 3: Commit**

```bash
git add docs/control-latency-reduction-design-2026-06-18.md docs/features/FEAT-0019-sidecar-persist-off-hot-path.md
git commit -m "docs(feat-0019): accept D-WRITEHOT-1 (identity-gated sidecar persist)"
```

---

### Task 1: Identity-gated synchronous persist in `PendingWritesStore::Upsert`

**Files:**
- Create: `tests/cpp/pending_writes_store_tests.cpp`
- Modify: `CMakeLists.txt:340` (register the new test after the power-anticipation test)
- Modify: `src/runtime/pending_writes.cpp:166-186` (`PendingWritesStore::Upsert`)
- Modify: `src/runtime/pending_writes.h:63-70, 95-97` (class + `Upsert` doc comments)

**Interfaces:**
- Consumes: `PendingWritesStore(std::filesystem::path)`,
  `void Upsert(const PendingWriteEntry&)`, `void QueueRemove(std::uint32_t)`,
  `void Flush()` (`src/runtime/pending_writes.h`);
  `std::vector<PendingWriteEntry> ReadPendingWrites(const std::filesystem::path&)`;
  `std::filesystem::path PendingWritesSidecarPath(const std::filesystem::path&)`.
- Produces: no signature change. The only behavioral change is *when* `Upsert`
  performs the synchronous disk write.

- [ ] **Step 1: Write the failing test file**

Create `tests/cpp/pending_writes_store_tests.cpp`. The persist detector is
file-existence: delete the on-disk sidecar, call `Upsert`, and check whether it
recreated the file (an identity-change persist rewrites the whole file
synchronously; a same-baseline change does not persist synchronously but is written
by a following `Flush()`).

```cpp
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

}  // namespace

int main() {
    TestSameBaselineTargetChangeDefersToFlush();
    TestBaselineChangePersistsSynchronously();
    TestNewChannelPersistsSynchronously();
    TestRecoveryBaselineRecordedAtActivation();
    TestRemovalIsFlushed();
    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "pending_writes_store_tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, add after the `svg_mb_control_power_anticipation_tests`
registration (around line 340):

```cmake
    svg_mb_control_add_core_test(svg_mb_control_pending_writes_store_tests
        tests/cpp/pending_writes_store_tests.cpp)
```

- [ ] **Step 3: Run the test to verify it FAILS**

Run: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`
Expected: the new `svg_mb_control_pending_writes_store_tests` FAILS —
`TestSameBaselineTargetChangeDefersToFlush` reports
`FAIL: same-baseline target change does not persist synchronously` because today's
`Upsert` always calls `Persist()`, so the sidecar is recreated.

- [ ] **Step 4: Implement the identity-gated persist**

Replace `PendingWritesStore::Upsert` in `src/runtime/pending_writes.cpp:166-186`
with:

```cpp
void PendingWritesStore::Upsert(const PendingWriteEntry& entry) {
    if (!loaded_) {
        Load();
    }
    bool replaced = false;
    bool baseline_changed = false;
    for (auto& existing : entries_) {
        if (existing.channel == entry.channel) {
            baseline_changed =
                existing.baseline_duty_raw != entry.baseline_duty_raw ||
                existing.baseline_mode_raw != entry.baseline_mode_raw;
            existing = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        entries_.push_back(entry);
    }
    // FEAT-0019 (REQ-WRITEHOT-01/04): persist synchronously only when the
    // recovery-relevant identity changes — a new channel entry (first
    // activation) or a changed captured baseline. ReconcilePendingWrites
    // restores from (channel, baseline_duty_raw, baseline_mode_raw) only and
    // never reads target_pct, so a same-baseline target_pct/hold/started_iso
    // change does not need a synchronous write before ApplyDuty: it marks the
    // store dirty and the existing end-of-tick Flush() writes it (off the hot
    // path). The in-memory entry is updated above either way, so the FEAT-0010
    // non-vetoing actuation path is unaffected.
    if (!replaced || baseline_changed) {
        Persist();  // clears dirty_
    } else {
        dirty_ = true;
    }
}
```

> REQ-WRITEHOT-06 (the FEAT-0010 counter interaction) is a separate edit — see
> Task 1b. It is split out because the persist-skip above makes a deferred `Upsert`
> return normally, which would otherwise let `channel_write.cpp` clear the
> persist-failure counter while a failed record is still missing.

- [ ] **Step 5: Update the `Upsert` / class doc comments**

In `src/runtime/pending_writes.h`, change the class comment (lines 63-70) and the
`Upsert` comment (lines 95-97) so they state the identity-gated rule instead of
"Upsert still persists synchronously on every call." Replace the `Upsert` comment
with:

```cpp
    // Inserts or replaces the entry for entry.channel. Persists the sidecar to
    // disk synchronously ONLY when the recovery-relevant identity changes — a
    // new channel entry (first activation) or a changed baseline_duty_raw /
    // baseline_mode_raw — so the crash-recovery record (sidecar reflects every
    // active channel's captured baseline before ApplyDuty) is preserved. A
    // same-baseline target_pct/hold/started_iso change marks the store dirty for
    // the next Flush() instead of writing synchronously (FEAT-0019), because those
    // fields are recovery-irrelevant. Throws on filesystem failure when it does
    // persist.
    void Upsert(const PendingWriteEntry& entry) override;
```

In the class comment block (lines 63-70), change "Upsert still persists
synchronously so the crash-recovery contract ... is preserved" to "Upsert persists
synchronously only when a channel's captured baseline changes (first activation or
re-capture); same-baseline target churn is deferred to Flush() (FEAT-0019)."

- [ ] **Step 6: Run the test to verify it PASSES (and nothing regressed)**

Run: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`
Expected: `svg_mb_control_pending_writes_store_tests` PASSES; the existing
`svg_mb_control_channel_write_tests` and the full CTest set stay green (the
FEAT-0010 throwing-store and reconcile tests are unaffected because the in-memory
update and the persist-on-throw fall-through are unchanged).

- [ ] **Step 7: Commit**

```bash
git add tests/cpp/pending_writes_store_tests.cpp CMakeLists.txt src/runtime/pending_writes.cpp src/runtime/pending_writes.h
git commit -m "feat(runtime): FEAT-0019 identity-gated sidecar persist off the actuation hot path (REQ-WRITEHOT-01/02/03/04)"
```

---

### Task 1b: Keep a deferred `Upsert` from corrupting the persist-failure health signal (REQ-WRITEHOT-06)

**Files:**
- Modify: `src/control/channel_write.cpp` (the FEAT-0010 `consecutive_sidecar_persist_failures` reset site)
- Likely modify: `src/runtime/pending_writes.{h,cpp}` + the FEAT-0010 store double in `tests/cpp/channel_write_tests.cpp` (depends on the §11 decision)
- Test: `tests/cpp/channel_write_tests.cpp`

**Interfaces:**
- Consumes: the Design-A `Upsert` from Task 1 (a same-baseline `Upsert` returns normally without persisting).
- Produces: a counter that reflects actual persist success.

> **Resolve FEAT-0019 §11 first.** This step depends on the open decision recorded in
> FEAT-0019 §11 (how `Upsert` signals whether it actually persisted: a `bool` return
> consumed by `channel_write.cpp`, or moving the reset into the store's
> successful-`Persist()` path). Pick the approach in the decision record before
> writing code.

The hazard (FEAT-0019 §4, REQ-WRITEHOT-06): after Task 1, a deferred same-baseline
`Upsert` returns normally, so today's unconditional
`channel.consecutive_sidecar_persist_failures = 0u` after `Upsert`
(`channel_write.cpp`) would clear the counter even though no persist ran — falsely
"healthy" while a failed activation record is still missing. Two failure modes to
avoid when implementing:

1. **False clear** — a deferred `Upsert` resets the counter (the primary bug).
2. **Stuck degraded** — if the reset moves to "only on a real persist," a failed
   activation whose record later self-heals via the end-of-tick `Flush()` (which has
   no channel context) leaves the counter degraded forever. The chosen mechanism
   must clear the counter when the record actually reaches disk, whether that write
   happened in `Upsert` or the subsequent `Flush()`.

- [ ] **Step 1: Write the failing test**

In `tests/cpp/channel_write_tests.cpp`, drive `TryApplyChannelSetpoint` (or the store
directly, per the chosen seam) so the activation persist fails, then issue a
same-baseline write, and assert `consecutive_sidecar_persist_failures` is **not**
reset to 0 by the deferred write. Reuse the FEAT-0010 `ThrowingPendingWritesStore`
pattern. Add a second case: once a persist succeeds, the counter clears.

- [ ] **Step 2: Run it to verify it FAILS** (today's reset clears unconditionally).

- [ ] **Step 3: Implement the chosen §11 mechanism** so the counter clears only when
  the record is actually persisted (in `Upsert` or the following `Flush()`), and not
  on a deferred-only `Upsert`. Update any `PendingWritesStoreInterface` implementers
  (incl. the FEAT-0010 test double) if the seam changes.

- [ ] **Step 4: Run the test to verify it PASSES** and the full CTest stays green
  (FEAT-0010 throwing-store + reconcile tests included).

- [ ] **Step 5: Commit**

```bash
git add src/control/channel_write.cpp src/runtime/pending_writes.h src/runtime/pending_writes.cpp tests/cpp/channel_write_tests.cpp
git commit -m "fix(control): FEAT-0019 deferred Upsert must not corrupt the persist-failure counter (REQ-WRITEHOT-06)"
```

---

### Task 2: Document the persist semantics (RUNTIME_HOME / WRITE_ORCHESTRATION)

**Files:**
- Modify: `docs/RUNTIME_HOME.md` (pending_writes.json field semantics)
- Modify: `docs/WRITE_ORCHESTRATION.md` (persist-frequency behavior)

**Interfaces:**
- Consumes: nothing in code; this is the `AGENTS.md` §Change Checklist doc step for a
  runtime-sidecar behavior change.
- Produces: the contract text REQ-WRITEHOT-05 review checks against.

- [ ] **Step 1: Clarify the sidecar field semantics in RUNTIME_HOME.md**

Find the `pending_writes.json` description in `docs/RUNTIME_HOME.md` and add: a
same-baseline change to `target_pct` / `started_iso` is written at the end-of-tick
`Flush()` rather than synchronously before `ApplyDuty`, so on disk they are at most
one tick stale and are advisory/diagnostic only. Crash recovery
(`ReconcilePendingWrites`) and the `--health` readability check do not read
`target_pct`; recovery restores from `baseline_duty_raw` / `baseline_mode_raw` only.
State that the schema (`schema_version = 1`) and fields are unchanged.

- [ ] **Step 2: Note the persist-frequency behavior in WRITE_ORCHESTRATION.md**

In `docs/WRITE_ORCHESTRATION.md`, add to the pending-writes/recovery section: the
control loop persists the sidecar synchronously before `ApplyDuty` only on a
recovery-relevant identity change (first activation or baseline re-capture);
same-baseline setpoint churn during a ramp is deferred to the existing once-per-tick
end-of-tick `Flush()` rather than persisted synchronously, so no fsync'd file-replace
runs before `ApplyDuty` during a ramp (FEAT-0019). The crash-recovery guarantee is
unchanged because the baseline is recorded synchronously at activation.

- [ ] **Step 3: Read back and commit**

Re-read both edited sections and `git diff` them (docs-only checklist,
`AGENTS.md` §Change Checklist).

```bash
git add docs/RUNTIME_HOME.md docs/WRITE_ORCHESTRATION.md
git commit -m "docs(feat-0019): pending_writes synchronous persist is identity-gated; target_pct deferred to Flush (<=1 tick stale)"
```

---

### Task 3: Close the spec — verification log, traceability, status, governance

**Files:**
- Modify: `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md` (header Status; §14 log)
- Modify: `docs/TRACEABILITY.md` (FEAT-0019 §3 result cells; §2 status row)
- Modify: `docs/features/README.md` (FEAT-0019 registry status)

**Interfaces:**
- Consumes: the passing CTest names from Task 1 (the verification evidence).
- Produces: a governance-consistent `Implemented` spec
  (`tests/test_feature_specs.py` green).

- [ ] **Step 1: Run the full local CI and capture the test result**

Run: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`
Expected: CTest all green including `svg_mb_control_pending_writes_store_tests`;
pytest green including `tests/test_feature_specs.py`. Record the CTest line (e.g.
"NN/NN") for the verification log.

- [ ] **Step 2: Fill the FEAT-0019 §14 verification log**

In `docs/features/FEAT-0019-sidecar-persist-off-hot-path.md` §14, set each row's
Result `pass`, Evidence (the `pending_writes_store_tests` case name per REQ for
01–04, the Task 1b counter test for REQ-WRITEHOT-06, plus the review note for
REQ-WRITEHOT-05), and the date. Set the §13 promotion gates to all
`[x]` (gate 3 was closed in Task 0). Flip the header **Status:** `Draft` →
`Implemented`. Add the **Spec vs. implementation deltas** note (e.g. "tests live in a
new `pending_writes_store_tests.cpp`; persist detected by file-existence").

- [ ] **Step 3: Mirror the results into TRACEABILITY.md**

In `docs/TRACEABILITY.md`, change each `REQ-WRITEHOT-0N` Result cell from
`not buildable` to `pass` (matching the §14 log — `test_traceability_results_match_implemented_verification_logs` requires the spec §14 result and the TRACEABILITY result to be identical for Implemented specs). Update the §2 `FEAT-0019` status row to `Implemented` with a one-line summary.

- [ ] **Step 4: Update the registry status**

In `docs/features/README.md`, change the FEAT-0019 registry-row Status from `Draft
(build-ready ...)` to `Implemented (2026-MM-DD; identity-gated sidecar persist; CTest green)`.

- [ ] **Step 5: Run the governance gate and full CI to verify consistency**

Run: `python -m pytest tests/test_feature_specs.py -q`
Expected: PASS (registry/spec/traceability status and REQ parity consistent for the
now-Implemented spec).
Run: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`
Expected: full CTest + pytest green.

- [ ] **Step 6: Commit**

```bash
git add docs/features/FEAT-0019-sidecar-persist-off-hot-path.md docs/TRACEABILITY.md docs/features/README.md
git commit -m "docs(feat-0019): mark Implemented; verification log + traceability filled (REQ-WRITEHOT-01..06)"
```

---

## Self-review

- **Spec coverage:** REQ-WRITEHOT-01 (Task 1 — defer/baseline/new-channel tests),
  REQ-WRITEHOT-02 (Task 1 `TestRecoveryBaselineRecordedAtActivation` + review),
  REQ-WRITEHOT-03 (Task 1 activation-persists tests; ordering vs ApplyDuty is the
  unchanged call site), REQ-WRITEHOT-04 (Task 1 `TestSameBaselineTargetChangeDefersToFlush`
  + baseline re-capture / removal tests), REQ-WRITEHOT-05 (Task 2 doc edits + review),
  REQ-WRITEHOT-06 (Task 1b counter test + the `channel_write.cpp` reset change). All
  six mapped.
- **Placeholder scan:** none — every code/test/cmake block is complete; the
  acceptance/commit dates are `2026-MM-DD` placeholders to fill at execution, and
  Task 1b's exact seam is intentionally deferred to the FEAT-0019 §11 decision.
- **Type consistency:** Task 1 keeps the `Upsert`/`Flush`/`QueueRemove` signatures
  unchanged; Task 1b may change the `Upsert` seam per the §11 decision (and must then
  update every `PendingWritesStoreInterface` implementer). `PendingWriteEntry` fields
  match `pending_writes.h`; `PendingWritesSidecarPath` and `ReadPendingWrites` used
  with their real signatures.

## Notes for the two gated siblings (not in this plan)

- **FEAT-0017** (response retune) and **FEAT-0018** (adaptive-cadence floor) are not
  code-ready: FEAT-0017 needs the lanes/target decision and a response-evaluation
  Pass-3 capture; FEAT-0018 **crosses the measurement gate** and needs the cadence
  characterization run first. Their "plan" is the evidence sequence in each spec's
  §12 plus the suggested order in `docs/next_steps.md` ("Control latency reduction").
  Promote them to their own implementation plans once their gates clear.
