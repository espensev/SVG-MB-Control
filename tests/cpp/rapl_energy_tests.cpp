// Unit tests for the pure RAPL package-energy math (src/hardware/rapl_energy.h),
// FEAT-0006 REQ-CPUEFF-02 verification. No hardware needed.

#include "rapl_energy.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void ExpectNear(double actual, double expected, double tolerance,
                const std::string& message) {
    if (std::fabs(actual - expected) > tolerance) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected " << expected << " got "
                  << actual << '\n';
    }
}

void ExpectTrue(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectFalse(bool condition, const std::string& message) {
    ExpectTrue(!condition, message);
}

using namespace svg_mb_control::rapl;

constexpr double kEps = 1e-4;
// Guard constants matching the validation plan / decision proposal.
constexpr double kCeilingW = 400.0;
constexpr double kDtCapMs = 3000.0;

void TestEnergyUnitDecode_Default() {
    // ESU = 16 (0b10000) in bits [12:8] -> 0x1000 raw -> 15.2588 uj/unit.
    ExpectNear(EnergyUnitMicrojoules(0x00001000u), 15.2587890625, kEps,
               "ESU=16 -> 15.2588 uj/unit");
    // The value observed live on the 9950X3D: raw 0x000a1000 -> ESU still 16.
    ExpectNear(EnergyUnitMicrojoules(0x000a1000u), 15.2587890625, kEps,
               "live raw 0x000a1000 decodes ESU=16");
}

void TestEnergyUnitDecode_OtherEsu() {
    // ESU = 17 -> half the unit.
    ExpectNear(EnergyUnitMicrojoules(0x00001100u), 7.62939453125, kEps,
               "ESU=17 -> 7.6294 uj/unit");
}

void TestUnitsDelta_Normal() {
    ExpectTrue(PackageEnergyUnitsDelta(1000u, 2000u) == 1000u,
               "normal forward delta == 1000");
}

void TestUnitsDelta_ExactlyOneWrap() {
    // prev near top, cur just past zero: modular delta crosses one 32-bit wrap.
    ExpectTrue(PackageEnergyUnitsDelta(0xFFFFFFF0ull, 0x00000010ull) == 0x20u,
               "one-wrap modular delta == 0x20");
}

void TestUnitsDelta_BackwardCounterIsHuge() {
    // A backward step (no real wrap) modular-subtracts to a near-2^32 value,
    // which the ceiling guard rejects -- never a false small/zero delta.
    const std::uint32_t d = PackageEnergyUnitsDelta(2000u, 1000u);
    ExpectTrue(d > 0xFF000000u, "backward counter -> huge modular delta");
}

void TestDeltaMicrojoules() {
    // 1000 units * 15.2588 uj = 15258.8 uj.
    ExpectNear(PackageEnergyDeltaMicrojoules(1000u, 2000u, 15.2587890625),
               15258.7890625, 1e-2, "1000 units -> ~15258.8 uj");
}

void TestAverageWatts() {
    // 1e8 uj over 1000 ms = 100 J/s = 100 W.
    ExpectNear(AveragePackageWatts(1.0e8, 1000.0), 100.0, kEps,
               "1e8 uj / 1000 ms == 100 W");
    ExpectTrue(std::isnan(AveragePackageWatts(1.0e8, 0.0)),
               "zero window -> NaN watts");
}

void TestGuard_PlausibleWindow() {
    // 100 W over ~1 s: in range, window ok -> not blanked.
    ExpectFalse(WindowImplausible(1.0e8, 1000.0, kCeilingW, kDtCapMs),
                "100 W / 1 s is plausible");
}

void TestGuard_HighPowerCeiling() {
    // 500 W over 1 s -> above the 400 W ceiling -> blanked.
    ExpectTrue(WindowImplausible(5.0e8, 1000.0, kCeilingW, kDtCapMs),
               "500 W / 1 s exceeds ceiling -> blank");
}

void TestGuard_DtBound() {
    // Missing / over-long windows are blanked regardless of apparent power.
    ExpectTrue(WindowImplausible(1.0e8, 0.0, kCeilingW, kDtCapMs),
               "zero window -> blank");
    ExpectTrue(WindowImplausible(1.0e8, 5000.0, kCeilingW, kDtCapMs),
               "5 s window > dt cap -> blank");
}

void TestGuard_MultiWrapAliasesToLowPower_IsBlankedByDt() {
    // The decision's worked case: true 200 W for 400 s multi-wraps and aliases
    // to ~36 W, which the ceiling cannot catch. The dt branch must blank it.
    const double uj_per_unit = 15.2587890625;
    // Simulate the modular units delta after 400 s at 200 W.
    // 200 W * 400 s = 80,000 J = 8e10 uj -> /unit = ~5.243e9 units; mod 2^32.
    const std::uint64_t total_units =
        static_cast<std::uint64_t>(80000.0 * 1.0e6 / uj_per_unit);
    const std::uint32_t modular = static_cast<std::uint32_t>(total_units);
    const double delta_uj = static_cast<double>(modular) * uj_per_unit;
    const double aliased_w = AveragePackageWatts(delta_uj, 400000.0);
    ExpectTrue(aliased_w < kCeilingW,
               "multi-wrap aliases below the ceiling (ceiling can't catch it)");
    ExpectTrue(WindowImplausible(delta_uj, 400000.0, kCeilingW, kDtCapMs),
               "multi-wrap long window is blanked by the dt branch");
}

void TestAdvanceWindow_Baseline() {
    bool have_prev = false;
    std::uint64_t prev = 0u;
    std::uint64_t id = 0u;
    const EnergyWindowResult r = AdvanceEnergyWindow(
        &have_prev, &prev, &id, 5000u, 1000.0, 15.2587890625, kCeilingW,
        kDtCapMs);
    ExpectTrue(r.baseline, "first call is baseline");
    ExpectTrue(have_prev, "baseline sets have_prev");
    ExpectTrue(prev == 5000u, "baseline stores prev_raw");
    ExpectTrue(id == 0u && r.sample_id == 0u, "baseline does not advance id");
    ExpectTrue(std::isnan(r.delta_uj) && std::isnan(r.window_ms),
               "baseline emits no delta/window");
}

void TestAdvanceWindow_FirstDeltaAndIncrement() {
    bool have_prev = true;
    std::uint64_t prev = 1000u;
    std::uint64_t id = 0u;
    const EnergyWindowResult r = AdvanceEnergyWindow(
        &have_prev, &prev, &id, 2000u, 1000.0, 15.2587890625, kCeilingW,
        kDtCapMs);
    ExpectFalse(r.baseline, "delta path is not baseline");
    ExpectTrue(id == 1u && r.sample_id == 1u, "id advances on first delta");
    ExpectTrue(prev == 2000u, "prev_raw updated to current");
    ExpectNear(r.delta_uj, 15258.7890625, 1e-2, "1000-unit delta in uj");
    ExpectNear(r.window_ms, 1000.0, kEps, "window carried through");
    const EnergyWindowResult r2 = AdvanceEnergyWindow(
        &have_prev, &prev, &id, 3000u, 1000.0, 15.2587890625, kCeilingW,
        kDtCapMs);
    ExpectTrue(id == 2u && r2.sample_id == 2u, "id advances again next window");
}

void TestAdvanceWindow_GuardBlanksDeltaButKeepsId() {
    bool have_prev = true;
    std::uint64_t prev = 1000u;
    std::uint64_t id = 5u;
    // Over-long window (> dt cap) blanks the delta, but the id still advances so
    // a consumer sees the window happened (no false zero).
    const EnergyWindowResult r = AdvanceEnergyWindow(
        &have_prev, &prev, &id, 2000u, 5000.0, 15.2587890625, kCeilingW,
        kDtCapMs);
    ExpectTrue(id == 6u && r.sample_id == 6u, "guard-blank still advances id");
    ExpectTrue(std::isnan(r.delta_uj), "guard blanks the delta (no false zero)");
}

void TestAdvanceWindow_Wrap() {
    bool have_prev = true;
    std::uint64_t prev = 0xFFFFFFF0ull;
    std::uint64_t id = 0u;
    const EnergyWindowResult r = AdvanceEnergyWindow(
        &have_prev, &prev, &id, 0x10ull, 1000.0, 15.2587890625, kCeilingW,
        kDtCapMs);
    ExpectNear(r.delta_uj, 32.0 * 15.2587890625, 1e-2,
               "modular wrap delta across a window");
}

void TestAllowlistGuard() {
    ExpectTrue(IsAllowlistedEnergyMsr(0xC0010299u), "RAPL pwr unit allow-listed");
    ExpectTrue(IsAllowlistedEnergyMsr(0xC001029Bu), "pkg energy allow-listed");
    ExpectFalse(IsAllowlistedEnergyMsr(0x000000E8u), "APERF not allow-listed");
    ExpectFalse(IsAllowlistedEnergyMsr(0x000000E7u), "MPERF not allow-listed");
    ExpectFalse(IsAllowlistedEnergyMsr(0xC0000103u), "TSC_AUX not allow-listed");
    ExpectFalse(IsAllowlistedEnergyMsr(0xC001029Au),
                "per-core energy not allow-listed");
    ExpectFalse(IsAllowlistedEnergyMsr(0xFFFFFFFFu), "bogus not allow-listed");
}

}  // namespace

int main() {
    TestAllowlistGuard();
    TestAdvanceWindow_Baseline();
    TestAdvanceWindow_FirstDeltaAndIncrement();
    TestAdvanceWindow_GuardBlanksDeltaButKeepsId();
    TestAdvanceWindow_Wrap();
    TestEnergyUnitDecode_Default();
    TestEnergyUnitDecode_OtherEsu();
    TestUnitsDelta_Normal();
    TestUnitsDelta_ExactlyOneWrap();
    TestUnitsDelta_BackwardCounterIsHuge();
    TestDeltaMicrojoules();
    TestAverageWatts();
    TestGuard_PlausibleWindow();
    TestGuard_HighPowerCeiling();
    TestGuard_DtBound();
    TestGuard_MultiWrapAliasesToLowPower_IsBlankedByDt();
    if (g_failures > 0) {
        std::cerr << g_failures << " rapl_energy test failure(s)\n";
        return 1;
    }
    std::cout << "rapl_energy tests OK\n";
    return 0;
}
