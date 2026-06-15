// Throwaway synthetic CPU load generator.
//
// Two read-only uses, one binary:
//   1. FEAT-0006 CPU-energy quarantine-exit capture
//      (docs/cpu-energy-quarantine-exit-capture-runbook): a clean, reproducible
//      idle -> load -> cooldown step with a steady sub-window for the external
//      (SMU) power cross-check. This is the original use; the default invocation
//      (no scheduling flags) is byte-identical to that behavior.
//   2. Layer-0 loop-stall reproduction instrument
//      (docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md): a controllable
//      antagonist for the 06-09 control-loop starvation, which does NOT reproduce
//      under a plain Normal-priority all-core load. The scheduling knobs below
//      (--priority, --pin, --oversubscribe) let an operator sweep the suspected
//      causes one at a time -- the load's priority class, its per-core affinity,
//      and thread oversubscription -- while the shipped BelowNormal controller is
//      held fixed, to find the minimal condition that stalls the loop.
//
// NOT shipped, NOT part of the control loop, NOT a dependency of any runtime path.
// Read-only on the machine: pure FP register math, no I/O, no MSR, no PCI, no fan
// writes. The scheduling knobs touch only THIS process's own priority/affinity
// (SetPriorityClass / SetThreadAffinityMask) -- they change nothing on the
// hardware or in the controller. REALTIME_PRIORITY_CLASS is intentionally NOT
// offered (it can preempt driver IOCTL / critical system threads and hard-lock the
// box). OFF by default at the CMake level.
//
// Each worker runs eight independent 256-bit FMA accumulator chains (re-seeded per
// outer iteration so values stay bounded and the compiler cannot fold the loop),
// saturating both FMA ports to pull near-package-max power (AVX2 -- a safer, lower-
// power proxy than a y-cruncher VT3 / AVX-512 power-virus). Stops after --seconds
// (self-terminating) or runs until killed when --seconds is 0.
//
// Usage:
//   cpu-synth-load.exe [--threads N] [--seconds S] [--oversubscribe K]
//                      [--priority below|normal|above|high] [--pin]
//     --threads        worker count (default: logical CPU count)
//     --seconds        run duration; 0 = until killed (default: 0)
//     --oversubscribe  set threads = K * logical CPUs (ignored if --threads given)
//     --priority       process priority class (default: inherit from launcher).
//                      below|normal|above|high -- realtime is not accepted.
//     --pin            pin worker t to logical CPU (t mod CPU count), so every core
//                      carries a busy thread (deterministic full occupancy)
//
// Build (OFF by default):
//   cmake -DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON ... \
//     && cmake --build ... --target cpu_synth_load

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>
#include <windows.h>  // SetPriorityClass / SetThreadAffinityMask (scheduling knobs)

namespace {

std::atomic<bool> g_stop{false};
std::atomic<std::uint64_t> g_work{0};

std::string NowIso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

// Pin the calling thread to a single logical CPU. Single processor group only
// (this box is 32 logical CPUs, well under the 64-per-group affinity-mask limit);
// no-op for core >= 64 rather than silently wrapping into the wrong group.
void PinThisThread(unsigned core) {
    if (core < 64u) {
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << core;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
}

void Worker(double seed, int pin_core) {
    if (pin_core >= 0) {
        PinThisThread(static_cast<unsigned>(pin_core));
    }
    const __m256d b = _mm256_set1_pd(1.0 + seed * 1e-12);
    const __m256d c = _mm256_set1_pd(seed * 1e-12);
    std::uint64_t local = 0;
    double keep = 0.0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        __m256d a0 = _mm256_set1_pd(1.0 + seed);
        __m256d a1 = _mm256_set1_pd(1.5 + seed);
        __m256d a2 = _mm256_set1_pd(2.0 + seed);
        __m256d a3 = _mm256_set1_pd(2.5 + seed);
        __m256d a4 = _mm256_set1_pd(3.0 + seed);
        __m256d a5 = _mm256_set1_pd(3.5 + seed);
        __m256d a6 = _mm256_set1_pd(4.0 + seed);
        __m256d a7 = _mm256_set1_pd(4.5 + seed);
        for (int i = 0; i < 2048; ++i) {
            a0 = _mm256_fmadd_pd(a0, b, c);
            a1 = _mm256_fmadd_pd(a1, b, c);
            a2 = _mm256_fmadd_pd(a2, b, c);
            a3 = _mm256_fmadd_pd(a3, b, c);
            a4 = _mm256_fmadd_pd(a4, b, c);
            a5 = _mm256_fmadd_pd(a5, b, c);
            a6 = _mm256_fmadd_pd(a6, b, c);
            a7 = _mm256_fmadd_pd(a7, b, c);
        }
        const __m256d s = _mm256_add_pd(
            _mm256_add_pd(_mm256_add_pd(a0, a1), _mm256_add_pd(a2, a3)),
            _mm256_add_pd(_mm256_add_pd(a4, a5), _mm256_add_pd(a6, a7)));
        double out[4];
        _mm256_storeu_pd(out, s);
        keep += out[0] + out[1] + out[2] + out[3];
        local += 2048ull * 8ull * 4ull;  // fmadd issues * chains * lanes
    }
    // Fold keep into the published counter so the loop cannot be optimized out.
    g_work.fetch_add(local + static_cast<std::uint64_t>(keep) % 7ull,
                     std::memory_order_relaxed);
}

// Apply a process priority class by name. REALTIME is intentionally rejected.
// Returns false on an unknown name or a failed SetPriorityClass call.
bool ApplyPriority(const std::string& name) {
    DWORD cls = 0;
    if (name == "below") {
        cls = BELOW_NORMAL_PRIORITY_CLASS;
    } else if (name == "normal") {
        cls = NORMAL_PRIORITY_CLASS;
    } else if (name == "above") {
        cls = ABOVE_NORMAL_PRIORITY_CLASS;
    } else if (name == "high") {
        cls = HIGH_PRIORITY_CLASS;
    } else {
        return false;
    }
    return SetPriorityClass(GetCurrentProcess(), cls) != 0;
}

const char* CurrentPriorityName() {
    switch (GetPriorityClass(GetCurrentProcess())) {
        case IDLE_PRIORITY_CLASS:
            return "idle";
        case BELOW_NORMAL_PRIORITY_CLASS:
            return "below";
        case NORMAL_PRIORITY_CLASS:
            return "normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return "above";
        case HIGH_PRIORITY_CLASS:
            return "high";
        case REALTIME_PRIORITY_CLASS:
            return "realtime";
        default:
            return "unknown";
    }
}

}  // namespace

int main(int argc, char** argv) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0u) {
        hw = 1u;
    }
    unsigned threads = hw;
    int seconds = 0;
    int oversub = 0;
    bool threads_explicit = false;
    bool pin = false;
    std::string priority;  // empty = inherit launcher priority (do not change it)

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
            threads_explicit = true;
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "--oversubscribe" && i + 1 < argc) {
            oversub = std::atoi(argv[++i]);
        } else if (a == "--priority" && i + 1 < argc) {
            priority = argv[++i];
        } else if (a == "--pin") {
            pin = true;
        } else {
            std::fprintf(stderr, "unknown/incomplete arg: %s\n", a.c_str());
            return 2;
        }
    }

    if (oversub > 0) {
        if (threads_explicit) {
            std::fprintf(stderr, "note: --threads overrides --oversubscribe\n");
        } else {
            threads = hw * static_cast<unsigned>(oversub);
        }
    }
    if (threads == 0u) {
        threads = 1u;
    }

    if (!priority.empty() && !ApplyPriority(priority)) {
        std::fprintf(stderr,
                     "bad/failed --priority '%s' (use below|normal|above|high)\n",
                     priority.c_str());
        return 2;
    }

    std::printf(
        "cpu_synth_load start threads=%u cores=%u (%.2fx) priority=%s pin=%s "
        "seconds=%d ts=%s\n",
        threads, hw, static_cast<double>(threads) / static_cast<double>(hw),
        CurrentPriorityName(), pin ? "on" : "off", seconds, NowIso().c_str());
    std::fflush(stdout);

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const int pin_core = pin ? static_cast<int>(t % hw) : -1;
        pool.emplace_back(Worker, 1.0 + static_cast<double>(t), pin_core);
    }

    if (seconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        g_stop.store(true, std::memory_order_relaxed);
    } else {
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    for (auto& th : pool) {
        th.join();
    }
    std::printf("cpu_synth_load stop ts=%s work=%llu\n", NowIso().c_str(),
                static_cast<unsigned long long>(g_work.load()));
    return 0;
}
