// FEAT-0008 (REQ-WATCHDOG-01/02/04): unit tests for the bounded force-terminate
// escalation orchestration. A FakeProcessTerminator stands in for the Win32
// process operations so the worker-first ordering, the PID-reuse image guard,
// the worker-PID corroboration guard, the single-shot bound, and the
// runtime_gone relaunch gate are exercised without spawning real processes.
//
// The single-handle race-free exit confirm and the relaunch-with-a-new-PID
// behaviour live in the Win32 adapter + --restart wiring and are covered by the
// Python integration test (a suspended worker drives the real path).

#include "test_helpers.h"

#include "worker_force_terminate.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

using svg_mb_control::ForceTerminateInputs;
using svg_mb_control::ForceTerminateOutcome;
using svg_mb_control::ForceTerminateResult;
using svg_mb_control::ProcessTerminator;
using svg_mb_control::RuntimeLogEvent;

const std::wstring kImage = L"C:\\release\\svg-mb-control.exe";

class FakeProcessTerminator : public ProcessTerminator {
 public:
    struct TerminateCall {
        std::uint32_t pid;
        std::wstring image;
        std::uint32_t timeout_ms;
    };
    struct WaitCall {
        std::uint32_t pid;
        std::wstring image;
        std::uint32_t timeout_ms;
    };

    std::vector<TerminateCall> terminate_calls;
    std::vector<WaitCall> wait_calls;
    // Scripted per-PID results. Missing PID -> kConfirmedGone / not-exited.
    std::map<std::uint32_t, ForceTerminateResult> terminate_results;
    std::map<std::uint32_t, bool> wait_results;

    ForceTerminateResult TerminateIfImageMatches(
        std::uint32_t pid, const std::wstring& image,
        std::uint32_t timeout_ms) override {
        terminate_calls.push_back({pid, image, timeout_ms});
        const auto it = terminate_results.find(pid);
        return it != terminate_results.end()
                   ? it->second
                   : ForceTerminateResult::kConfirmedGone;
    }

    bool WaitForExit(std::uint32_t pid, const std::wstring& image,
                     std::uint32_t timeout_ms) override {
        wait_calls.push_back({pid, image, timeout_ms});
        const auto it = wait_results.find(pid);
        return it != wait_results.end() ? it->second : false;
    }

    std::size_t TerminateCallsFor(std::uint32_t pid) const {
        std::size_t n = 0;
        for (const auto& call : terminate_calls) {
            if (call.pid == pid) {
                ++n;
            }
        }
        return n;
    }
};

bool HasEvent(const std::vector<RuntimeLogEvent>& events,
              const std::string& event_type) {
    for (const auto& e : events) {
        if (e.event_type == event_type) {
            return true;
        }
    }
    return false;
}

const RuntimeLogEvent* FindEvent(const std::vector<RuntimeLogEvent>& events,
                                 const std::string& event_type) {
    for (const auto& e : events) {
        if (e.event_type == event_type) {
            return &e;
        }
    }
    return nullptr;
}

ForceTerminateInputs MakeInputs(std::uint32_t worker_pid,
                                std::uint32_t supervisor_pid) {
    ForceTerminateInputs in;
    in.worker_pid = worker_pid;
    in.worker_pid_runtime = worker_pid;  // corroborated by default
    in.supervisor_pid = supervisor_pid;
    in.expected_image_path = kImage;
    in.graceful_stop_result = 2;
    return in;
}

constexpr std::uint32_t kConfirmMs = 3000u;
constexpr char kWorkerKilled[] = "supervisor.worker_force_terminated";
constexpr char kSupervisorKilled[] = "supervisor.supervisor_force_terminated";
constexpr char kFailed[] = "supervisor.force_terminate_failed";

// T1: hung worker, supervisor self-exits once the worker dies (the D1 happy
// path). Worker is force-terminated, supervisor is NOT, runtime is gone.
void TestHungWorkerSupervisorSelfExits() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kConfirmedGone;
    fake.wait_results[2000] = true;  // supervisor self-exited within the window

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(MakeInputs(1000, 2000), fake,
                                               kConfirmMs);

    ExpectTrue(out.runtime_gone, "T1: runtime confirmed gone");
    ExpectTrue(fake.TerminateCallsFor(1000) == 1, "T1: worker terminated once");
    ExpectTrue(fake.TerminateCallsFor(2000) == 0,
               "T1: supervisor not force-terminated (it self-exited)");
    ExpectTrue(!fake.terminate_calls.empty() &&
                   fake.terminate_calls.front().pid == 1000,
               "T1: worker is killed first");
    ExpectTrue(!fake.terminate_calls.empty() &&
                   fake.terminate_calls.front().image == kImage,
               "T1: worker terminate uses the expected image guard");
    ExpectTrue(HasEvent(out.events, kWorkerKilled),
               "T1: worker_force_terminated event emitted");
    const RuntimeLogEvent* ev = FindEvent(out.events, kWorkerKilled);
    ExpectTrue(ev != nullptr &&
                   ev->detail.find("1000") != std::string::npos,
               "T1: event records the terminated worker PID");
    ExpectTrue(ev != nullptr && !ev->detail.empty(),
               "T1: event carries a reason detail");
}

// T2: the worker PID's live image is not svg-mb-control.exe (PID reuse). The
// guard refuses to kill it and the escalation bails -- nothing else is touched.
void TestImageGuardRefusesWorker() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kImageMismatch;

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(MakeInputs(1000, 2000), fake,
                                               kConfirmMs);

    ExpectFalse(out.runtime_gone, "T2: not gone -- guard refused");
    ExpectTrue(fake.TerminateCallsFor(1000) == 1, "T2: worker attempted once");
    ExpectTrue(fake.TerminateCallsFor(2000) == 0,
               "T2: supervisor not touched after a worker guard refusal");
    ExpectTrue(fake.wait_calls.empty(),
               "T2: no supervisor self-exit wait after refusal");
    ExpectTrue(HasEvent(out.events, kFailed),
               "T2: force_terminate_failed event emitted");
}

// T3: control_supervisor.json worker PID and control_runtime.json process_id
// disagree -> the worker PID is uncorroborated; refuse before touching the OS.
void TestPidCorroborationConflictRefuses() {
    FakeProcessTerminator fake;
    ForceTerminateInputs in = MakeInputs(1000, 2000);
    in.worker_pid_runtime = 1234u;  // conflicts with worker_pid=1000

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(in, fake, kConfirmMs);

    ExpectFalse(out.runtime_gone, "T3: not gone -- uncorroborated PID");
    ExpectTrue(fake.terminate_calls.empty(),
               "T3: no process terminated on PID disagreement");
    ExpectTrue(fake.wait_calls.empty(), "T3: no wait on PID disagreement");
    ExpectTrue(HasEvent(out.events, kFailed),
               "T3: force_terminate_failed event emitted");
}

// T4: the supervisor is itself wedged (does not self-exit) -> force-kill it too.
void TestSupervisorWedgedAlsoForceKilled() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kConfirmedGone;
    fake.wait_results[2000] = false;  // supervisor still alive after the window
    fake.terminate_results[2000] = ForceTerminateResult::kConfirmedGone;

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(MakeInputs(1000, 2000), fake,
                                               kConfirmMs);

    ExpectTrue(out.runtime_gone, "T4: runtime confirmed gone");
    ExpectTrue(fake.TerminateCallsFor(1000) == 1, "T4: worker terminated once");
    ExpectTrue(fake.TerminateCallsFor(2000) == 1,
               "T4: supervisor force-terminated once");
    ExpectTrue(fake.terminate_calls.size() == 2 &&
                   fake.terminate_calls[0].pid == 1000 &&
                   fake.terminate_calls[1].pid == 2000,
               "T4: worker killed before supervisor");
    ExpectTrue(!fake.wait_calls.empty() && fake.wait_calls.front().pid == 2000,
               "T4: supervisor given a self-exit window first");
    ExpectTrue(HasEvent(out.events, kWorkerKilled),
               "T4: worker_force_terminated event emitted");
    ExpectTrue(HasEvent(out.events, kSupervisorKilled),
               "T4: supervisor_force_terminated event emitted");
}

// T5: TerminateProcess succeeds but exit is not confirmed within the budget.
// The escalation is single-shot -- it does NOT retry, and reports not-gone.
void TestConfirmTimeoutIsBoundedNoRetry() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kConfirmTimeout;

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(MakeInputs(1000, 2000), fake,
                                               kConfirmMs);

    ExpectFalse(out.runtime_gone, "T5: not gone on confirm timeout");
    ExpectTrue(fake.TerminateCallsFor(1000) == 1,
               "T5: exactly one worker attempt (no retry loop)");
    ExpectTrue(fake.TerminateCallsFor(2000) == 0,
               "T5: supervisor not touched when the worker is unconfirmed");
    ExpectTrue(HasEvent(out.events, kFailed),
               "T5: force_terminate_failed event emitted");
}

// T6: the worker already exited between detection and escalation. No worker
// kill event is fabricated, but with the supervisor gone the runtime is gone.
void TestWorkerAlreadyGone() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kNoSuchProcess;
    fake.wait_results[2000] = true;

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(MakeInputs(1000, 2000), fake,
                                               kConfirmMs);

    ExpectTrue(out.runtime_gone, "T6: runtime gone (worker already exited)");
    ExpectFalse(HasEvent(out.events, kWorkerKilled),
                "T6: no fabricated worker_force_terminated event");
    ExpectFalse(HasEvent(out.events, kFailed),
                "T6: an already-gone worker is not a failure");
}

// T7: no worker PID resolved at all -> nothing to act on, reported as failed.
void TestNoWorkerPidFails() {
    FakeProcessTerminator fake;
    ForceTerminateInputs in = MakeInputs(0u, 2000);
    in.worker_pid_runtime.reset();

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(in, fake, kConfirmMs);

    ExpectFalse(out.runtime_gone, "T7: not gone with no worker PID");
    ExpectTrue(fake.terminate_calls.empty(), "T7: nothing terminated");
    ExpectTrue(HasEvent(out.events, kFailed),
               "T7: force_terminate_failed event emitted");
}

// T8: control_runtime.json was unreadable (no corroboration available) -> the
// supervisor sidecar PID is authoritative; the escalation proceeds.
void TestMissingRuntimeCorroborationProceeds() {
    FakeProcessTerminator fake;
    fake.terminate_results[1000] = ForceTerminateResult::kConfirmedGone;
    fake.wait_results[2000] = true;
    ForceTerminateInputs in = MakeInputs(1000, 2000);
    in.worker_pid_runtime.reset();  // control_runtime.json absent/unreadable

    const ForceTerminateOutcome out =
        svg_mb_control::EscalateForceTerminate(in, fake, kConfirmMs);

    ExpectTrue(out.runtime_gone, "T8: proceeds when corroboration is absent");
    ExpectTrue(fake.TerminateCallsFor(1000) == 1, "T8: worker terminated once");
}

}  // namespace

int main() {
    TestHungWorkerSupervisorSelfExits();
    TestImageGuardRefusesWorker();
    TestPidCorroborationConflictRefuses();
    TestSupervisorWedgedAlsoForceKilled();
    TestConfirmTimeoutIsBoundedNoRetry();
    TestWorkerAlreadyGone();
    TestNoWorkerPidFails();
    TestMissingRuntimeCorroborationProceeds();

    if (g_failures == 0) {
        std::cout << "worker_force_terminate_tests: all assertions passed\n";
    }
    return g_failures;
}
