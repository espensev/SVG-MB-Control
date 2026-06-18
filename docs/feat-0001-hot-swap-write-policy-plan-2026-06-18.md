# FEAT-0001 Hot-swap runtime write policy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a running controller change its write policy (`writes_enabled`,
`blocked_channels`) at a tick boundary via a runtime-home request file, by
rebuilding the `FanWriter` from the new policy (build-then-swap) so the snapshot
gate and the hardware gate can never drift.

**Architecture:** The control loop already owns the `FanWriter` as a
`std::unique_ptr` (`src/control/control_loop.cpp:89-91`) and the
`RuntimeWritePolicy` lives in `ControlRuntimeContext` (`context.runtime_policy`,
read each tick by `SampleDirectRuntimeSnapshot`). The change adds (1) a
`write_policy.request.json` request file + helpers mirroring the breaker-reset
request, (2) a per-tick intake `ProcessWritePolicyChangeRequest` mirroring
`ProcessCircuitBreakerResetRequest` (`src/control/tick_runner.cpp:36-128`) that
runs right after the breaker-reset intake, and (3) a writer-ownership signature
change so the intake can rebuild and swap the writer. On a valid change the intake
builds a fresh writer from the new policy (`CreateFanWriter`), restores baselines
for channels the new policy blocks (the existing `RestoreSavedState` path,
`src/control/channel_write.cpp:204-238`), swaps in the new writer + policy, and
suppresses writes for that one transition tick.

**Tech Stack:** C++17, CMake/CTest, the `tests/cpp/test_helpers.h` assertion
harness (`ExpectTrue`/`ExpectEqual`/`ExpectFalse`, `g_failures`, per-file
`main()`), `nlohmann::json` via `WriteJsonFileAtomic`, the simulated `FanWriter`
(`src/hardware/simulated_fan_writer.cpp`, selected by `CreateFanWriter`).

## Global Constraints

- Spec: `docs/features/FEAT-0001-hot-swap-write-policy.md` (`REQ-WRITEPOLICY-01..09`).
  Decision record `docs/write-policy-hotswap-decision-2026-06-03.md` is **Accepted**
  (gate 3 already cleared); the spec is `Accepted`, i.e. build-authorized.
- **No `third_party/SVG-MB-SIO` API change** (REQ-WRITEPOLICY-02). Build-then-swap
  reuses the existing `CreateFanWriter` construction path; `MbSioController` keeps
  `init(policy)` + read-only accessors only.
- **Additive runtime-home schema only** (`docs/RUNTIME_HOME.md`): the new request
  file and status fields are additive; absence means "no change." Existing
  runtime-home files and `config/runtime_policy_*.json` stay valid; live changes
  are in-memory only (startup config remains the durable source).
- **Measurement Gate** (`docs/MEASUREMENT_GATE.md`): the *disable / block*
  direction reduces authority and is always safe; the *enable / unblock* direction
  adds a live channel and **must** be gated (REQ-WRITEPOLICY-07).
- Preserve the FEAT-0010 invariant: do not add a throw before `ApplyDuty` on the
  per-channel path.
- **Overlap with FEAT-0019 (parallel work):** FEAT-0019 edits
  `src/runtime/pending_writes.{h,cpp}` and the persist-failure-counter reset region
  of `src/control/channel_write.cpp`. This plan stays out of that region (it reuses
  `RestoreSavedState` only) and puts all new tests in a **new** file
  `tests/cpp/write_policy_hotswap_tests.cpp` (not `channel_write_tests.cpp`) to avoid
  test-file conflicts. The only shared files are `CMakeLists.txt`,
  `docs/TRACEABILITY.md`, `docs/features/README.md`, `docs/RUNTIME_HOME.md`,
  `docs/WRITE_ORCHESTRATION.md` (line-level, resolvable at merge).
- Build/validate with `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (no publish, no
  live runtime interaction — `AGENTS.md` §Live Runtime Safety).
- Doctrine (`CLAUDE.md`): grounded claims, correct `must`/`should`/`is`, no
  undefined terms in doc updates.

## File structure

| File | Responsibility | Create/Modify |
|---|---|---|
| `src/runtime/runtime_lifecycle.h` / `.cpp` | `RuntimeWritePolicyRequest` struct + `...RequestPath` / `RequestRuntimeWritePolicy` / `TakeRuntimeWritePolicyRequest` / `ClearRuntimeWritePolicyRequest`, mirroring the breaker-reset helpers | Modify |
| `src/control/tick_runner.cpp` / `.h` | `ProcessWritePolicyChangeRequest` intake + build-then-swap; `RunControlTick` signature change to `std::unique_ptr<FanWriter>&`; transition-tick write-skip | Modify |
| `src/control/control_loop.cpp` | Pass the owned `unique_ptr` (not `*fan_writer`) into `RunControlTick` | Modify (`:176`) |
| `src/control/control_runtime_context.h` (or `control_loop_run_state`) | `bool write_policy_changed_this_tick` run-state flag | Modify |
| `src/runtime/runtime_status.{h,cpp}` | Additive effective-write-policy status fields | Modify |
| `src/app/app_args.cpp` (+ the breaker-reset operator subcommand neighbor) | New `--set-write-policy` operator subcommand writing the request file | Modify |
| `tests/cpp/write_policy_hotswap_tests.cpp` | All FEAT-0001 unit tests (new file — overlap mitigation) | Create |
| `CMakeLists.txt` | Register the new test | Modify |
| `docs/RUNTIME_HOME.md`, `docs/WRITE_ORCHESTRATION.md`, `README.md` | Request file, status fields, events, operator workflow | Modify |
| `docs/features/FEAT-0001-hot-swap-write-policy.md`, `docs/TRACEABILITY.md`, `docs/features/README.md` | §14 log, results, status `Accepted`→`Implemented` | Modify |

---

### Task 1: `write_policy.request.json` lifecycle helpers

**Files:**
- Modify: `src/runtime/runtime_lifecycle.h` (after `RuntimeBreakerResetRequest`, lines 10-36)
- Modify: `src/runtime/runtime_lifecycle.cpp` (mirror the breaker-reset Path/Request/Take/Clear impls)
- Create: `tests/cpp/write_policy_hotswap_tests.cpp` (first cases)
- Modify: `CMakeLists.txt` (register the new test)

**Interfaces:**
- Produces:
  ```cpp
  struct RuntimeWritePolicyRequest {
      std::optional<bool> writes_enabled;                 // absent = unchanged
      std::optional<std::vector<std::uint32_t>> blocked_channels;  // absent = unchanged
      bool acknowledge_measurement_gate = false;          // required to unblock/enable (REQ-07)
      std::string requested_at;
      std::string parse_error;                            // non-empty => invalid request
  };
  std::filesystem::path RuntimeWritePolicyRequestPath(const std::filesystem::path& runtime_home);
  bool RequestRuntimeWritePolicy(const std::filesystem::path& runtime_home,
                                 const RuntimeWritePolicyRequest& request);
  std::optional<RuntimeWritePolicyRequest> TakeRuntimeWritePolicyRequest(const std::filesystem::path& runtime_home);
  void ClearRuntimeWritePolicyRequest(const std::filesystem::path& runtime_home);
  ```
- Consumes: `WriteJsonFileAtomic`, `nlohmann::json`, the breaker-reset impls in `runtime_lifecycle.cpp` as the exact pattern to copy (file name `"write_policy.request.json"` at the runtime-home root, matching the flat `*.request.json` convention).

- [ ] **Step 1: Write the failing round-trip test**

Create `tests/cpp/write_policy_hotswap_tests.cpp`:

```cpp
// Tests for FEAT-0001: hot-swap runtime write policy.
#include "test_helpers.h"

#include "runtime_lifecycle.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

using svg_mb_control::RuntimeWritePolicyRequest;
using svg_mb_control::RequestRuntimeWritePolicy;
using svg_mb_control::TakeRuntimeWritePolicyRequest;
using svg_mb_control::RuntimeWritePolicyRequestPath;

std::filesystem::path MakeTempHome(const char* name) {
    std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        (std::string("svg_mb_control_write_policy_tests_") + name + "_" +
         UniqueTempSuffix());
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::create_directories(home, ec);
    return home;
}

// REQ-WRITEPOLICY-04 (intake) + schema: a written request round-trips and the
// take consumes (deletes) the file.
void TestRequestRoundTripsAndIsConsumed() {
    const std::filesystem::path home = MakeTempHome("roundtrip");
    RuntimeWritePolicyRequest req;
    req.writes_enabled = false;
    req.blocked_channels = std::vector<std::uint32_t>{2u, 5u};
    ExpectTrue(RequestRuntimeWritePolicy(home, req), "request file written");

    std::error_code ec;
    ExpectTrue(std::filesystem::exists(RuntimeWritePolicyRequestPath(home), ec),
               "request file exists before take");

    const auto taken = TakeRuntimeWritePolicyRequest(home);
    ExpectTrue(taken.has_value(), "take returns the request");
    ExpectTrue(taken->parse_error.empty(), "valid request has no parse error");
    ExpectTrue(taken->writes_enabled.has_value() && *taken->writes_enabled == false,
               "writes_enabled round-trips");
    ExpectTrue(taken->blocked_channels.has_value() &&
                   taken->blocked_channels->size() == 2u,
               "blocked_channels round-trips");
    ExpectFalse(std::filesystem::exists(RuntimeWritePolicyRequestPath(home), ec),
                "take consumes (removes) the request file");
}

// Absence of the file means no request (no change).
void TestNoRequestReturnsNullopt() {
    const std::filesystem::path home = MakeTempHome("absent");
    ExpectFalse(TakeRuntimeWritePolicyRequest(home).has_value(),
                "no file => nullopt");
}

}  // namespace

int main() {
    TestRequestRoundTripsAndIsConsumed();
    TestNoRequestReturnsNullopt();
    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "write_policy_hotswap_tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, next to the other `svg_mb_control_add_core_test(...)` lines:

```cmake
    svg_mb_control_add_core_test(svg_mb_control_write_policy_hotswap_tests
        tests/cpp/write_policy_hotswap_tests.cpp)
```

- [ ] **Step 3: Run to verify it FAILS** — `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`. Expected: link/compile failure (`RuntimeWritePolicyRequest` undefined).

- [ ] **Step 4: Add the struct + declarations to `runtime_lifecycle.h`** (the Interfaces block above), after `RuntimeBreakerResetRequest`.

- [ ] **Step 5: Implement Path/Request/Take/Clear in `runtime_lifecycle.cpp`** by copying the breaker-reset implementations and substituting the filename `"write_policy.request.json"` and the JSON fields (`writes_enabled` optional bool; `blocked_channels` optional array of uint; `acknowledge_measurement_gate` optional bool default false; `requested_at`). On malformed JSON set `parse_error` (do not throw), exactly as the breaker-reset take does.

- [ ] **Step 6: Run to verify it PASSES** — `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`; the two new cases pass; full CTest stays green.

- [ ] **Step 7: Commit**

```bash
git add tests/cpp/write_policy_hotswap_tests.cpp CMakeLists.txt src/runtime/runtime_lifecycle.h src/runtime/runtime_lifecycle.cpp
git commit -m "feat(runtime): FEAT-0001 write_policy.request.json lifecycle helpers (REQ-WRITEPOLICY-04 intake)"
```

---

### Task 2: Make the control loop's `FanWriter` swappable (ownership signature change)

**Files:**
- Modify: `src/control/tick_runner.h` (the `RunControlTick` declaration, `:85`)
- Modify: `src/control/tick_runner.cpp` (`RunControlTick` definition `:132-142`; deref at call sites inside)
- Modify: `src/control/control_loop.cpp` (`:176` — pass the `unique_ptr`)

**Interfaces:**
- Produces: `RunControlTick(..., std::unique_ptr<FanWriter>& fan_writer, ...)` — same parameter list otherwise. Inside, callees that take `FanWriter&` (e.g. `SampleDirectRuntimeSnapshot`, `HandleExpiredHoldRestore`, `TryApplyChannelSetpoint`) receive `*fan_writer`.
- Consumes: `control_loop.cpp` owns `std::unique_ptr<FanWriter> fan_writer` (`:89`).

This is a behavior-preserving refactor; it is verified by the **existing** CTest
staying green (no new test). It is its own task because it is the structural
precondition for Task 3 and a reviewer can gate it independently.

- [ ] **Step 1: Change the declaration + definition signature** from `FanWriter& fan_writer` to `std::unique_ptr<FanWriter>& fan_writer` in `tick_runner.h:85` and `tick_runner.cpp:132-142`.

- [ ] **Step 2: Dereference at the internal call sites.** In `RunControlTick`, change `fan_writer` to `*fan_writer` where a `FanWriter&` is expected (the `SampleDirectRuntimeSnapshot` call `:157`, the `HandleExpiredHoldRestore` call `:228-237`, the `TryApplyChannelSetpoint` call `:246-251`, and any other in the per-channel loop below `:251`). Add `#include <memory>` to `tick_runner.cpp`.

- [ ] **Step 3: Update the caller.** In `control_loop.cpp:176`, pass `fan_writer` (the `unique_ptr`) instead of `*fan_writer`.

- [ ] **Step 4: Run the full CTest to verify NOTHING regressed** — `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`. Expected: green; behavior identical (the writer is the same object, only passed by owning handle).

- [ ] **Step 5: Commit**

```bash
git add src/control/tick_runner.h src/control/tick_runner.cpp src/control/control_loop.cpp
git commit -m "refactor(control): RunControlTick takes the owned unique_ptr<FanWriter> so it can be swapped (FEAT-0001 precursor)"
```

---

### Task 3: Write-policy intake + build-then-swap

**Files:**
- Modify: `src/control/tick_runner.cpp` (new `ProcessWritePolicyChangeRequest` in the anonymous namespace; call it after `ProcessCircuitBreakerResetRequest` at `:195`; add the transition-tick write-skip in the per-channel loop)
- Modify: the run-state struct holding `ControlLoopRunState` (add `bool write_policy_changed_this_tick = false;`)
- Modify: `tests/cpp/write_policy_hotswap_tests.cpp` (REQ-03/05/06/08/09 cases, driven through a simulated writer + a temp runtime-home)

**Interfaces:**
- Consumes: `TakeRuntimeWritePolicyRequest` (Task 1); `CreateFanWriter(const RuntimeWritePolicy&)` → `std::unique_ptr<FanWriter>` (`src/hardware/fan_writer.h:135`); `RuntimeWritePolicyBlocksChannel` (`src/runtime/runtime_write_policy.h:28`); `fan_writer->RestoreSavedState(channel, baseline_duty_raw, baseline_mode_raw, timeout)` (`src/control/channel_write.cpp:204-208`); `AppendControlLoopEvent`.
- Produces: `void ProcessWritePolicyChangeRequest(ControlRuntimeContext& context, std::unique_ptr<FanWriter>& fan_writer, PendingWritesStore& pending_store, std::uint64_t tick_count, ControlLoopRunState& state);`

**Apply order (spec §5, build-then-swap):**
1. `Take` the request; if none, return. If `parse_error` non-empty → emit `control_loop.write_policy_invalid`, return.
2. Compute `RuntimeWritePolicy new_policy = context.runtime_policy;` then apply the request deltas (`writes_enabled`, `blocked_channels`).
3. **Measurement-gate guard (REQ-07):** if the change *unblocks* any channel (in old `blocked_channels`, not in new) **or** flips `writes_enabled` false→true, and `acknowledge_measurement_gate` is false → emit `control_loop.write_policy_rejected` (reason: "enable/unblock crosses the measurement gate; set acknowledge_measurement_gate"), return. The block/disable direction is always allowed.
4. **Build:** `std::unique_ptr<FanWriter> new_writer; try { new_writer = CreateFanWriter(new_policy); } catch (const std::exception& e) { emit control_loop.write_policy_rejected (REQ-03/08, detail e.what()); return; }` — old writer + policy untouched.
5. **Restore (old writer):** for each `channel` in `context.channels` with `channel.write_active` where the new policy blocks it (`RuntimeWritePolicyBlocksChannel(new_policy, ch)`) or `new_policy.writes_enabled == false`: call `fan_writer->RestoreSavedState(...)`, on success set `write_active = false`, `last_issued_pct = NaN`, best-effort `pending_store.QueueRemove(...)` (mirror `channel_write.cpp:204-238`).
6. **Swap:** `fan_writer = std::move(new_writer); context.runtime_policy = new_policy;`
7. `state.write_policy_changed_this_tick = true; state.force_status_write = true;` and emit `control_loop.write_policy_applied` (REQ-09) recording the delta.

**Transition-tick skip (REQ-06):** in `RunControlTick`'s per-channel loop, guard the
`TryApplyChannelSetpoint` call with `if (!state.write_policy_changed_this_tick)`; still
run `CaptureChannelBaselineIfAvailable` and `HandleExpiredHoldRestore`. Reset the flag
to `false` at the top of `RunControlTick` each tick.

- [ ] **Step 1: Write the failing tests** in `write_policy_hotswap_tests.cpp`. Build a `ControlRuntimeContext` with a simulated writer and 2-3 channels (reuse the construction helpers from `tests/cpp/channel_write_tests.cpp` — copy the minimal `MakeContext`/`MakeReadyChannel` shape into this file; do not include the other file). Cases:
  - `TestBlockWhileWriteActiveRestoresAndClears` (REQ-05): a `write_active` channel that the new policy blocks is restored (simulated writer records a `RestoreSavedState` call) and `write_active` becomes false.
  - `TestFailedRebuildRetainsPriorWriterAndPolicy` (REQ-03/08): inject a policy whose `CreateFanWriter` throws (use a sentinel that the simulated factory rejects, or a `blocked_channels` value the test harness maps to a throw) → the prior `context.runtime_policy` is unchanged and a `write_policy_rejected` event is logged.
  - `TestUnblockWithoutAckIsRejected` (REQ-07): a request unblocking a channel without `acknowledge_measurement_gate` → policy unchanged + `write_policy_rejected`.
  - `TestNoWriteOnTransitionTick` (REQ-06): after an applied change, the per-channel write is skipped for that tick (the simulated writer records no `ApplyDuty` on the transition tick) and resumes next tick.
  - `TestAppliedEmitsEvent` (REQ-09): a valid block/disable change emits `control_loop.write_policy_applied`. Use the event-log read helper pattern from `channel_write_tests.cpp` (`EventLogContains`).

- [ ] **Step 2: Run to verify they FAIL** (`ProcessWritePolicyChangeRequest` undefined / no skip).

- [ ] **Step 3: Implement `ProcessWritePolicyChangeRequest`** per the apply order above, in the `tick_runner.cpp` anonymous namespace, and wire the call + the transition-tick skip + the flag reset into `RunControlTick`.

- [ ] **Step 4: Run to verify they PASS** and the full CTest stays green (the FEAT-0010 throwing-store and reconcile tests are unaffected — this path adds no throw before `ApplyDuty`).

- [ ] **Step 5: Commit**

```bash
git add src/control/tick_runner.cpp src/control/tick_runner.h tests/cpp/write_policy_hotswap_tests.cpp
git commit -m "feat(control): FEAT-0001 write-policy intake + build-then-swap (REQ-WRITEPOLICY-01/03/05/06/08/09)"
```

---

### Task 4: Effective-write-policy status fields + operator subcommand

**Files:**
- Modify: `src/runtime/runtime_status.{h,cpp}` (additive fields: `effective_writes_enabled`, `effective_blocked_channels`) populated from `context.runtime_policy`
- Modify: `src/app/app_args.cpp` (+ the breaker-reset operator subcommand neighbor): `--set-write-policy [--writes-enabled true|false] [--block CH ...] [--acknowledge-measurement-gate]` that calls `RequestRuntimeWritePolicy`
- Modify: `tests/cpp/write_policy_hotswap_tests.cpp` (status-field assertion; reuse the snapshot/status build path)

**Interfaces:**
- Consumes: `RequestRuntimeWritePolicy` (Task 1); the existing breaker-reset CLI handler as the pattern (locate it via the `RequestRuntimeBreakerReset` call site in `src/app/`).

- [ ] **Step 1: Write the failing status test** — after an applied change, the runtime status snapshot reports the new effective policy.
- [ ] **Step 2: Run to verify it FAILS.**
- [ ] **Step 3: Add the additive status fields** in `runtime_status.{h,cpp}`, populated from `context.runtime_policy` where the snapshot/status is built.
- [ ] **Step 4: Add the `--set-write-policy` subcommand** in `app_args.cpp`, mirroring the breaker-reset operator subcommand; it writes the request file and exits (REQ operator surface, §8).
- [ ] **Step 5: Run to verify it PASSES** and full CTest green; manually confirm `svg-mb-control --set-write-policy --writes-enabled false` writes `write_policy.request.json`.
- [ ] **Step 6: Commit**

```bash
git add src/runtime/runtime_status.h src/runtime/runtime_status.cpp src/app/app_args.cpp tests/cpp/write_policy_hotswap_tests.cpp
git commit -m "feat(runtime,app): FEAT-0001 effective-write-policy status fields + --set-write-policy operator subcommand"
```

---

### Task 5: Docs — RUNTIME_HOME, WRITE_ORCHESTRATION, README

**Files:**
- Modify: `docs/RUNTIME_HOME.md` (the `write_policy.request.json` request file; the new status fields; absence = no change; in-memory-only semantics)
- Modify: `docs/WRITE_ORCHESTRATION.md` (the build-then-swap apply order; single policy owner; transition-tick semantics; the three `control_loop.write_policy_*` events)
- Modify: `README.md` (operator workflow: the `--set-write-policy` action and that unblock requires the measurement-gate acknowledgment)

- [ ] **Step 1: RUNTIME_HOME.md** — document the request file fields and the additive status fields; state existing files stay valid (REQ-WRITEPOLICY-01/04 review home).
- [ ] **Step 2: WRITE_ORCHESTRATION.md** — document the single-owner + build-then-swap + transition-tick + events (REQ-WRITEPOLICY-01/02 review home; REQ-06 transition).
- [ ] **Step 3: README.md** — the operator action + the measurement-gate acknowledgment for unblock.
- [ ] **Step 4: Re-read each edited section, `git diff`, commit.**

```bash
git add docs/RUNTIME_HOME.md docs/WRITE_ORCHESTRATION.md README.md
git commit -m "docs(feat-0001): write-policy request file, build-then-swap, events, operator workflow"
```

---

### Task 6: Close the spec — verification log, traceability, status, governance

**Files:**
- Modify: `docs/features/FEAT-0001-hot-swap-write-policy.md` (header Status; §14 log)
- Modify: `docs/TRACEABILITY.md` (FEAT-0001 §3 result cells `pending`→`pass`/`partial`; §2 status row)
- Modify: `docs/features/README.md` (FEAT-0001 registry status `Accepted`→`Implemented`)

- [ ] **Step 1: Run full local CI and capture the CTest line** — `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`. Record "NN/NN".
- [ ] **Step 2: Fill FEAT-0001 §14** — each row `pass` with the `write_policy_hotswap_tests` case name (REQ-01 review + the single-owner refactor; REQ-02 `CreateFanWriter` rebuild + no-vendored-API review; REQ-03/05/06/08/09 the Task-3 cases; REQ-04 the Task-1 round-trip). **REQ-WRITEPOLICY-07 is `partial`**: the reject-without-ack path is `T` (tested), but the live-unblock `M` runtime evidence is deferred (it requires a Measurement-Gate characterization run on hardware) — record `partial` with that note, matching the legend. Bump **Updated** to the implementation date; add the **Spec vs. implementation deltas** note (new test file; the `acknowledge_measurement_gate` request field as the REQ-07 enforcement seam; the transition-tick write-skip mechanism).
- [ ] **Step 3: Mirror results into TRACEABILITY.md** — each `REQ-WRITEPOLICY-0N` Result cell to match §14 (`pass`, with REQ-07 `partial`). `test_traceability_results_match_implemented_verification_logs` requires the §14 result and the TRACEABILITY result to be **identical** for Implemented specs. Update the §2 `FEAT-0001` status row to `Implemented`.
- [ ] **Step 4: Update the registry** — `docs/features/README.md` FEAT-0001 row `Accepted` → `Implemented (2026-MM-DD; build-then-swap write-policy hot-swap; CTest green; REQ-07 live unblock deferred to a measurement-gate run)`.
- [ ] **Step 5: Run governance + full CI** — `python -m unittest tests.test_feature_specs` (5/5) and `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (green).
- [ ] **Step 6: Commit**

```bash
git add docs/features/FEAT-0001-hot-swap-write-policy.md docs/TRACEABILITY.md docs/features/README.md
git commit -m "docs(feat-0001): mark Implemented; verification log + traceability (REQ-WRITEPOLICY-01..09; 07 partial)"
```

---

## Self-review

- **Spec coverage:** REQ-01 (Task 2 single-owner refactor + Task 3 swap; review), REQ-02
  (Task 3 `CreateFanWriter` rebuild + review no vendored API change), REQ-03 (Task 3
  `TestFailedRebuildRetainsPriorWriterAndPolicy`), REQ-04 (Task 1 round-trip + Task 3
  per-tick intake), REQ-05 (Task 3 `TestBlockWhileWriteActiveRestoresAndClears`), REQ-06
  (Task 3 `TestNoWriteOnTransitionTick` + the skip), REQ-07 (Task 3
  `TestUnblockWithoutAckIsRejected` = T; live `M` deferred → §14 `partial`), REQ-08 (Task 3
  failed-rebuild case), REQ-09 (Task 3 `TestAppliedEmitsEvent`). All nine mapped.
- **Placeholder scan:** the commit/registry dates are `2026-MM-DD` to fill at execution;
  the simulated-`CreateFanWriter`-throw seam in Task 3 Step 1 is named as "a sentinel the
  test factory rejects" — resolve it against `simulated_fan_writer.cpp` at execution (it is
  the one detail that depends on the live test factory).
- **Type consistency:** `RuntimeWritePolicyRequest` fields are used identically in Tasks 1,
  3, 4; `RunControlTick`'s `std::unique_ptr<FanWriter>&` is introduced in Task 2 and consumed
  in Task 3; `ProcessWritePolicyChangeRequest`'s signature matches its call site;
  `RestoreSavedState` and `RuntimeWritePolicyBlocksChannel` use their real signatures.
- **Overlap with FEAT-0019:** no edit to `pending_writes.*` or the `channel_write.cpp`
  persist-counter region; tests in a new file; shared files limited to `CMakeLists.txt` + the
  doc set (line-level, resolvable).

## Measurement-gate note (REQ-WRITEPOLICY-07)

The *block / disable* direction ships fully tested. The *unblock / enable* direction is
enforced-but-deferred: the code rejects it unless `acknowledge_measurement_gate` is set, and
the **live** runtime evidence for an actual unblock (`M`) is left for a Measurement-Gate
characterization run on hardware — recorded as `partial` in §14/TRACEABILITY, not claimed as
done. This keeps FEAT-0001 honest about what is verified versus what still needs a live run.
