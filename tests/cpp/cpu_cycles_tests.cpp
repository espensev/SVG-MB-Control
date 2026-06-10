// Unit tests for the pure APERF/MPERF cycle math (src/hardware/cpu_cycles.h),
// FEAT-0006 REQ-CPUEFF-01/03 verification. No hardware needed.

#include "cpu_cycles.h"

#include "test_helpers.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using namespace svg_mb_control::cycles;

constexpr double kEps = 1e-4;
// Guard constants for the tests (the live values are passed by amd_reader).
constexpr double kDtCapMs = 3000.0;
constexpr double kRatioCap = 4.0;

void TestAllowlistGuard() {
    ExpectTrue(IsAllowlistedCycleMsr(0xC00000E7u), "MPERF_RO allow-listed");
    ExpectTrue(IsAllowlistedCycleMsr(0xC00000E8u), "APERF_RO allow-listed");
    // The architectural indices the 2026-06-07 probe wrongly read are NOT
    // allow-listed by this module -- that was the whole bug.
    ExpectFalse(IsAllowlistedCycleMsr(0x000000E7u), "IA32_MPERF not allow-listed");
    ExpectFalse(IsAllowlistedCycleMsr(0x000000E8u), "IA32_APERF not allow-listed");
    ExpectFalse(IsAllowlistedCycleMsr(0xC001029Bu), "pkg energy not a cycle MSR");
    ExpectFalse(IsAllowlistedCycleMsr(0xC0000103u), "TSC_AUX not allow-listed");
    ExpectFalse(IsAllowlistedCycleMsr(0xFFFFFFFFu), "bogus not allow-listed");
}

void TestCyclesEnabledFromMode() {
    ExpectTrue(CyclesEnabledFromMode("enabled"), "\"enabled\" enables the read");
    ExpectFalse(CyclesEnabledFromMode("disabled"), "\"disabled\" stays off");
    ExpectFalse(CyclesEnabledFromMode(""), "empty/unset stays off");
    ExpectFalse(CyclesEnabledFromMode("Enabled"), "case-sensitive: stays off");
    ExpectFalse(CyclesEnabledFromMode("enabled "), "no trimming: stays off");
}

void TestCounterDelta() {
    ExpectTrue(CounterDelta(1000u, 3000u) == 2000u, "normal forward delta");
    // 64-bit wraparound: prev near top, cur just past zero.
    ExpectTrue(CounterDelta(0xFFFFFFFFFFFFFFF0ull, 0x0Full) == 0x1Full,
               "64-bit modular wrap delta");
}

void TestRatio() {
    ExpectNear(AperfMperfRatio(2746900132ull, 2145984445ull), 1.280, 1e-3,
               "live busy ratio ~1.280");
    ExpectTrue(std::isnan(AperfMperfRatio(1000u, 0u)),
               "zero MPERF -> NaN ratio (no false value)");
}

void TestEffectiveFrequency() {
    // ratio 1.28 * 4300 MHz base -> ~5504 MHz (within the 9950X3D boost range).
    ExpectNear(EffectiveFrequencyMhz(5504u, 4300u, 4300.0), 5504.0, 1.0,
               "ratio*base -> effective MHz");
    ExpectTrue(std::isnan(EffectiveFrequencyMhz(5000u, 4000u, 0.0)),
               "non-positive base -> NaN");
    ExpectTrue(std::isnan(EffectiveFrequencyMhz(5000u, 0u, 4300.0)),
               "zero MPERF -> NaN effective freq");
}

void TestGuard_Plausible() {
    // ratio ~1.28 over ~1 s: in range, window ok -> not blanked.
    ExpectFalse(CycleWindowImplausible(2746900132ull, 2145984445ull, 1000.0,
                                       kDtCapMs, kRatioCap),
                "live busy window is plausible");
}

void TestGuard_BlankCases() {
    ExpectTrue(CycleWindowImplausible(1000u, 0u, 1000.0, kDtCapMs, kRatioCap),
               "zero MPERF (no C0 work) -> blank");
    ExpectTrue(CycleWindowImplausible(1000u, 1000u, 0.0, kDtCapMs, kRatioCap),
               "zero window -> blank");
    ExpectTrue(CycleWindowImplausible(1000u, 1000u, 5000.0, kDtCapMs, kRatioCap),
               "over-long window > dt cap -> blank");
    // ratio 10x base (cross-core straddle / garbage) exceeds the cap.
    ExpectTrue(CycleWindowImplausible(10000u, 1000u, 1000.0, kDtCapMs, kRatioCap),
               "ratio above cap (straddle/garbage) -> blank");
}

void TestAdvanceWindow_Baseline() {
    bool have_prev = false;
    std::uint64_t prev_a = 0u;
    std::uint64_t prev_m = 0u;
    std::uint64_t id = 0u;
    const CycleWindowResult r =
        AdvanceCycleWindow(&have_prev, &prev_a, &prev_m, &id, 5000u, 4000u,
                           1000.0, kDtCapMs, kRatioCap);
    ExpectTrue(r.baseline, "first call is baseline");
    ExpectTrue(have_prev, "baseline sets have_prev");
    ExpectTrue(prev_a == 5000u && prev_m == 4000u, "baseline stores prev");
    ExpectTrue(id == 0u && r.sample_id == 0u, "baseline does not advance id");
    ExpectFalse(r.blanked, "baseline is not a blank");
}

void TestAdvanceWindow_DeltaAndIncrement() {
    bool have_prev = true;
    std::uint64_t prev_a = 1000u;
    std::uint64_t prev_m = 1000u;
    std::uint64_t id = 0u;
    // cur: +2560 APERF, +2000 MPERF -> ratio 1.28, plausible.
    const CycleWindowResult r =
        AdvanceCycleWindow(&have_prev, &prev_a, &prev_m, &id, 3560u, 3000u,
                           1000.0, kDtCapMs, kRatioCap);
    ExpectFalse(r.baseline, "delta path is not baseline");
    ExpectFalse(r.blanked, "plausible window is not blanked");
    ExpectTrue(id == 1u && r.sample_id == 1u, "id advances on first delta");
    ExpectTrue(r.d_aperf == 2560u && r.d_mperf == 2000u, "modular deltas");
    ExpectTrue(prev_a == 3560u && prev_m == 3000u, "prev updated to current");
    ExpectNear(AperfMperfRatio(r.d_aperf, r.d_mperf), 1.28, 1e-3, "ratio 1.28");
}

void TestAdvanceWindow_GuardBlanksButKeepsId() {
    bool have_prev = true;
    std::uint64_t prev_a = 1000u;
    std::uint64_t prev_m = 1000u;
    std::uint64_t id = 5u;
    // Over-long window blanks the deltas, but the id still advances.
    const CycleWindowResult r =
        AdvanceCycleWindow(&have_prev, &prev_a, &prev_m, &id, 3560u, 3000u,
                           5000.0, kDtCapMs, kRatioCap);
    ExpectTrue(id == 6u && r.sample_id == 6u, "guard-blank still advances id");
    ExpectTrue(r.blanked, "over-long window is blanked");
    ExpectTrue(r.d_aperf == 0u && r.d_mperf == 0u,
               "blanked deltas left 0 (caller emits empty, not zero)");
}

void TestAdvanceWindow_Wrap() {
    bool have_prev = true;
    std::uint64_t prev_a = 0xFFFFFFFFFFFFFFF0ull;
    std::uint64_t prev_m = 1000u;
    std::uint64_t id = 0u;
    const CycleWindowResult r =
        AdvanceCycleWindow(&have_prev, &prev_a, &prev_m, &id, 0x10ull, 3000u,
                           1000.0, kDtCapMs, kRatioCap);
    ExpectTrue(r.d_aperf == 0x20ull, "64-bit modular wrap delta for APERF");
    ExpectTrue(r.d_mperf == 2000u, "MPERF delta across the window");
}

}  // namespace

int main() {
    TestAllowlistGuard();
    TestCyclesEnabledFromMode();
    TestCounterDelta();
    TestRatio();
    TestEffectiveFrequency();
    TestGuard_Plausible();
    TestGuard_BlankCases();
    TestAdvanceWindow_Baseline();
    TestAdvanceWindow_DeltaAndIncrement();
    TestAdvanceWindow_GuardBlanksButKeepsId();
    TestAdvanceWindow_Wrap();
    if (g_failures > 0) {
        std::cerr << g_failures << " cpu_cycles test failure(s)\n";
        return 1;
    }
    std::cout << "cpu_cycles tests OK\n";
    return 0;
}
