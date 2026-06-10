// Throwaway synthetic CPU load generator for the FEAT-0006 CPU-energy
// quarantine-exit capture (docs/cpu-energy-quarantine-exit-capture-runbook).
//
// Drives a controlled, reproducible all-core load so a capture session has a
// clean idle -> load -> cooldown step and a steady sub-window for the external
// (SMU) power cross-check. NOT shipped, NOT part of the control loop, NOT a
// dependency of any runtime path: read-only on the machine (pure FP register
// math, no I/O, no MSR, no fan writes). OFF by default at the CMake level.
//
// Each worker runs eight independent 256-bit FMA accumulator chains (re-seeded
// per outer iteration so values stay bounded and the compiler cannot fold the
// loop), saturating both FMA ports to pull near-package-max power. Stops after
// --seconds (self-terminating; the orchestrator launches it with a duration)
// or runs until killed when --seconds is 0.
//
// Usage:
//   cpu-synth-load.exe [--threads N] [--seconds S]
//     --threads  worker count (default: hardware_concurrency)
//     --seconds  run duration; 0 = until killed (default: 0)
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

void Worker(double seed) {
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

}  // namespace

int main(int argc, char** argv) {
    unsigned threads = std::thread::hardware_concurrency();
    int seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "unknown/incomplete arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (threads == 0u) {
        threads = 1u;
    }

    std::printf("cpu_synth_load start threads=%u seconds=%d ts=%s\n", threads,
                seconds, NowIso().c_str());
    std::fflush(stdout);

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back(Worker, 1.0 + static_cast<double>(t));
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
