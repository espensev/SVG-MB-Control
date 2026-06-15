// Throwaway constant-occupancy spike load generator for the proposed transient
// power-anticipation ("spike catcher") Gate-2 capture
// (docs/cpu-transient-power-anticipation-plan-2026-06-15.md §8 case 1 / §10 Q1).
//
// Produces the load case cpu_synth_load.cpp cannot: a SUSTAINED ~99 % busy
// plateau at LOW package power, with AVX2 power BURSTS injected at CONSTANT
// occupancy (busy% does not move; package watts surges). That is the regime
// where system_cpu_busy_pct is blind and only power/current sees the event.
//
// Constant occupancy is the whole point: one persistent worker pool runs the
// entire time and never sleeps, so the OS sees ~100 % busy throughout. An atomic
// phase flag flips every worker between two kernels:
//   - FLOOR: a per-thread dependent pointer-chase over a DRAM-resident buffer
//     (memory-latency bound). The core is occupied (busy% pinned) but stalls on
//     memory most cycles, so package power is low.
//   - BURST: eight 256-bit FMA accumulator chains (the cpu_synth_load kernel),
//     saturating both FMA ports to pull near-package-max power.
// The floor->burst transition is an atomic flag flip, so the package-power edge
// is sharp -- the thing the spike catcher must react to. Every transition is
// timestamped to stdout and (optionally) to a marker CSV, giving the earliness
// metric a ground-truth zero (plan §8).
//
// NOT shipped, NOT part of the control loop, NOT a dependency of any runtime
// path: read-only on the machine (FP register math + private-buffer loads; no
// MSR, no fan writes, no PCI). OFF by default at the CMake level. This authorizes
// no control-path work; it is capture tooling, a sibling of cpu_synth_load.cpp.
//
// Usage:
//   cpu-synth-spike-load.exe [--threads N] [--floor-seconds F] [--burst-seconds B]
//                            [--gap-seconds G] [--bursts K] [--buffer-mb M]
//                            [--marker-file PATH]
//     --threads        worker count (default: hardware_concurrency)
//     --floor-seconds  initial low-power plateau before the first burst (default 120)
//     --burst-seconds  duration of each AVX2 burst (default 20)
//     --gap-seconds    low-power gap after each burst (default 40)
//     --bursts         number of bursts; 0 = pure floor = the steady-control
//                      session (plan §8 case 2) (default 3)
//     --buffer-mb      per-thread pointer-chase buffer in MiB; larger = more
//                      DRAM-bound = lower floor power (default 32)
//     --marker-file    optional CSV: event,iso,unix_ms for each phase transition
//   Total run = floor-seconds + bursts * (burst-seconds + gap-seconds).
//
// Build (OFF by default):
//   cmake -DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON ... \
//     && cmake --build ... --target cpu_synth_spike_load

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>

namespace {

enum Phase { kFloor = 0, kBurst = 1 };

std::atomic<bool> g_stop{false};
std::atomic<int> g_phase{kFloor};
std::atomic<std::uint64_t> g_work{0};

std::string NowIso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

std::uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count());
}

// Build a single Hamiltonian cycle over [0,n) so that following next[] visits
// every slot once in random order -> dependent, cache-missing loads. Fixed seed:
// reproducible, no time/rand dependence.
void InitChase(std::vector<std::uint32_t>& next, std::uint32_t seed) {
    const std::uint32_t n = static_cast<std::uint32_t>(next.size());
    std::vector<std::uint32_t> order(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::mt19937 rng(0xC0FFEEu ^ seed);
    for (std::uint32_t i = n - 1; i > 0; --i) {
        const std::uint32_t j = rng() % (i + 1u);
        const std::uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        next[order[k]] = order[(k + 1u) % n];
    }
}

void Worker(double seed, std::vector<std::uint32_t>* chase) {
    const __m256d b = _mm256_set1_pd(1.0 + seed * 1e-12);
    const __m256d c = _mm256_set1_pd(seed * 1e-12);
    const std::uint32_t* next = chase->data();
    std::uint32_t ci = static_cast<std::uint32_t>(
        static_cast<std::size_t>(seed) % chase->size());
    std::uint64_t local = 0;
    double keep = 0.0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (g_phase.load(std::memory_order_relaxed) == kBurst) {
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
            local += 2048ull * 8ull * 4ull;
        } else {
            // Dependent pointer-chase: ~65536 loads, each address from the last.
            for (int i = 0; i < 65536; ++i) {
                ci = next[ci];
            }
            keep += static_cast<double>(ci) * 1e-9;
            local += 65536ull;
        }
    }
    g_work.fetch_add(local + static_cast<std::uint64_t>(keep) % 7ull,
                     std::memory_order_relaxed);
}

struct Marker {
    std::string event;
    std::string iso;
    std::uint64_t unix_ms;
};

void Emit(std::vector<Marker>& log, const char* event) {
    Marker m{event, NowIso(), NowUnixMs()};
    std::printf("spike_load %-11s ts=%s unix_ms=%llu\n", event, m.iso.c_str(),
                static_cast<unsigned long long>(m.unix_ms));
    std::fflush(stdout);
    log.push_back(std::move(m));
}

void SleepSeconds(int seconds) {
    for (int s = 0; s < seconds && !g_stop.load(std::memory_order_relaxed); ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace

int main(int argc, char** argv) {
    unsigned threads = std::thread::hardware_concurrency();
    int floor_seconds = 120;
    int burst_seconds = 20;
    int gap_seconds = 40;
    int bursts = 3;
    int buffer_mb = 32;
    std::string marker_file;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (a == "--floor-seconds" && i + 1 < argc) {
            floor_seconds = std::atoi(argv[++i]);
        } else if (a == "--burst-seconds" && i + 1 < argc) {
            burst_seconds = std::atoi(argv[++i]);
        } else if (a == "--gap-seconds" && i + 1 < argc) {
            gap_seconds = std::atoi(argv[++i]);
        } else if (a == "--bursts" && i + 1 < argc) {
            bursts = std::atoi(argv[++i]);
        } else if (a == "--buffer-mb" && i + 1 < argc) {
            buffer_mb = std::atoi(argv[++i]);
        } else if (a == "--marker-file" && i + 1 < argc) {
            marker_file = argv[++i];
        } else {
            std::fprintf(stderr, "unknown/incomplete arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (threads == 0u) {
        threads = 1u;
    }
    if (buffer_mb < 1) {
        buffer_mb = 1;
    }
    if (bursts < 0) {
        bursts = 0;
    }

    const std::size_t slots =
        static_cast<std::size_t>(buffer_mb) * 1024u * 1024u / sizeof(std::uint32_t);
    std::printf(
        "cpu_synth_spike_load start threads=%u floor=%ds burst=%ds gap=%ds "
        "bursts=%d buffer=%dMiB ts=%s\n",
        threads, floor_seconds, burst_seconds, gap_seconds, bursts, buffer_mb,
        NowIso().c_str());
    std::fflush(stdout);

    std::vector<std::vector<std::uint32_t>> chase(threads);
    for (unsigned t = 0; t < threads; ++t) {
        chase[t].resize(slots);
        InitChase(chase[t], t + 1u);
    }

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back(Worker, 1.0 + static_cast<double>(t), &chase[t]);
    }

    std::vector<Marker> log;
    Emit(log, "floor");                 // initial low-power plateau
    SleepSeconds(floor_seconds);
    for (int k = 0; k < bursts; ++k) {
        g_phase.store(kBurst, std::memory_order_relaxed);
        Emit(log, "burst_start");
        SleepSeconds(burst_seconds);
        g_phase.store(kFloor, std::memory_order_relaxed);
        Emit(log, "burst_end");
        SleepSeconds(gap_seconds);
    }
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& th : pool) {
        th.join();
    }
    Emit(log, "stop");

    if (!marker_file.empty()) {
        std::FILE* fp = nullptr;
        if (fopen_s(&fp, marker_file.c_str(), "w") == 0 && fp != nullptr) {
            std::fprintf(fp, "event,iso,unix_ms\n");
            for (const Marker& m : log) {
                std::fprintf(fp, "%s,%s,%llu\n", m.event.c_str(), m.iso.c_str(),
                             static_cast<unsigned long long>(m.unix_ms));
            }
            std::fclose(fp);
            std::printf("cpu_synth_spike_load markers -> %s\n",
                        marker_file.c_str());
        } else {
            std::fprintf(stderr, "could not write marker file: %s\n",
                         marker_file.c_str());
        }
    }
    std::printf("cpu_synth_spike_load stop ts=%s work=%llu\n", NowIso().c_str(),
                static_cast<unsigned long long>(g_work.load()));
    return 0;
}
