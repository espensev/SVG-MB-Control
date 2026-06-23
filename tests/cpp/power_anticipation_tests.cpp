// Unit tests for the pure power-anticipation stage math
// (src/control/power_anticipation.h, docs/archive/cpu-power-feedforward-plan-2026-06-10.md
// §4). Core-linked for control_math's SmoothStep. The stage is design support
// only -- nothing in the runtime calls it; these tests pin the intended
// semantics, in particular decay-on-unavailable (the deliberate difference
// from UpdateBoostStage's hold-on-undefined).

#include "power_anticipation.h"

#include "test_helpers.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace svg_mb_control::power_anticipation;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEps = 1e-9;

PowerAnticipationConfig ValidConfig() {
    PowerAnticipationConfig cfg;
    cfg.start_w = 120.0;
    cfg.full_w = 200.0;
    cfg.rise_pct_per_sec = 4.0;
    cfg.fall_pct_per_sec = 2.0;
    cfg.max_boost_pct = 10.0;
    return cfg;
}

void TestWattsFromWindow() {
    // 200,000,000 uJ over 1000 ms = 200 W.
    ExpectNear(PackageWattsFromWindow(200000000.0, 1000.0), 200.0, kEps,
               "200e6 uJ / 1 s -> 200 W");
    // One live-shape window: ~45 W idle over a 1004 ms window.
    ExpectNear(PackageWattsFromWindow(45180000.0, 1004.0), 45.0, 0.1,
               "idle-shape window -> ~45 W");
    ExpectNear(PackageWattsFromWindow(0.0, 1000.0), 0.0, kEps,
               "zero delta is a valid 0 W reading");
    ExpectTrue(std::isnan(PackageWattsFromWindow(1000.0, 0.0)),
               "zero window -> NaN");
    ExpectTrue(std::isnan(PackageWattsFromWindow(1000.0, -5.0)),
               "negative window -> NaN");
    ExpectTrue(std::isnan(PackageWattsFromWindow(kNaN, 1000.0)),
               "blanked delta -> NaN");
    ExpectTrue(std::isnan(PackageWattsFromWindow(-1.0, 1000.0)),
               "negative delta -> NaN");
}

void TestEma() {
    ExpectNear(EmaUpdate(kNaN, 100.0, 0.5), 100.0, kEps,
               "NaN prev seeds from sample");
    ExpectNear(EmaUpdate(100.0, 200.0, 0.5), 150.0, kEps, "alpha 0.5 midpoint");
    ExpectNear(EmaUpdate(100.0, 200.0, 1.0), 200.0, kEps,
               "alpha 1.0 -> sample");
    ExpectNear(EmaUpdate(100.0, 200.0, 0.0), 200.0, kEps,
               "alpha 0 invalid -> no smoothing");
    ExpectNear(EmaUpdate(100.0, 200.0, 2.0), 200.0, kEps,
               "alpha > 1 invalid -> no smoothing");
    ExpectTrue(std::isnan(EmaUpdate(100.0, kNaN, 0.5)),
               "NaN sample resets to NaN (fail-safe)");
}

void TestWattsInput_NoWindowYet() {
    WattsInputState s;
    ExpectTrue(std::isnan(UpdateWattsInput(&s, 0u, 200000000.0, 1000.0, 8u)),
               "sample_id 0 (no completed window) -> NaN");
}

void TestWattsInput_NewWindowAndHold() {
    WattsInputState s;
    ExpectNear(UpdateWattsInput(&s, 1u, 200000000.0, 1000.0, 8u), 200.0, kEps,
               "first window computes watts");
    // Mirrored ticks within the same window hold the value.
    for (int i = 0; i < 3; ++i) {
        ExpectNear(UpdateWattsInput(&s, 1u, 200000000.0, 1000.0, 8u), 200.0,
                   kEps, "held tick returns same watts");
    }
    ExpectTrue(s.ticks_held == 3u, "hold counter advanced");
    ExpectNear(UpdateWattsInput(&s, 2u, 100000000.0, 1000.0, 8u), 100.0, kEps,
               "new window refreshes watts");
    ExpectTrue(s.ticks_held == 0u, "new window resets hold counter");
}

void TestWattsInput_StaleAgesOut() {
    WattsInputState s;
    UpdateWattsInput(&s, 1u, 200000000.0, 1000.0, 2u);
    ExpectTrue(!std::isnan(UpdateWattsInput(&s, 1u, 0.0, 0.0, 2u)),
               "hold tick 1 still valid");
    ExpectTrue(!std::isnan(UpdateWattsInput(&s, 1u, 0.0, 0.0, 2u)),
               "hold tick 2 still valid");
    ExpectTrue(std::isnan(UpdateWattsInput(&s, 1u, 0.0, 0.0, 2u)),
               "hold tick 3 exceeds max_hold_ticks -> stale NaN");
}

void TestWattsInput_BlankWindowPropagates() {
    WattsInputState s;
    UpdateWattsInput(&s, 1u, 200000000.0, 1000.0, 8u);
    ExpectTrue(std::isnan(UpdateWattsInput(&s, 2u, kNaN, 1000.0, 8u)),
               "blanked window -> NaN immediately (no stale 200 W)");
}

void TestGuard_DisabledConfigs() {
    const double rising_watts = 500.0;
    PowerAnticipationConfig cfg = ValidConfig();
    cfg.start_w = kNaN;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, rising_watts), 0.0,
               kEps, "missing start_w disables (returns 0)");
    cfg = ValidConfig();
    cfg.full_w = kNaN;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, rising_watts), 0.0,
               kEps, "missing full_w disables");
    cfg = ValidConfig();
    cfg.rise_pct_per_sec = 0.0;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, rising_watts), 0.0,
               kEps, "non-positive rise disables");
    cfg = ValidConfig();
    cfg.fall_pct_per_sec = -1.0;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, rising_watts), 0.0,
               kEps, "negative fall disables");
    cfg = ValidConfig();
    cfg.max_boost_pct = 0.0;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, rising_watts), 0.0,
               kEps, "non-positive max disables");
    // Zero fall is valid (no decay), matching UpdateBoostStage.
    cfg = ValidConfig();
    cfg.fall_pct_per_sec = 0.0;
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, kNaN), 5.0, kEps,
               "zero fall rate: NaN input decays at rate 0 (holds value)");
}

void TestRiseScale() {
    const PowerAnticipationConfig cfg = ValidConfig();
    // At/above full_w: scale 1 -> rise = 4 %/s * 1 s.
    ExpectNear(UpdatePowerAnticipation(cfg, 0.0, 1000u, 200.0), 4.0, kEps,
               "full-band rise 4.0 over 1 s");
    // Midband 160 W: smootherstep(0.5) = 0.5 -> rise 2.0.
    ExpectNear(UpdatePowerAnticipation(cfg, 0.0, 1000u, 160.0), 2.0, kEps,
               "midband rise scaled by S5(0.5)=0.5");
    // Just at start_w: scale S5(0) = 0 -> no rise (but no decay either).
    ExpectNear(UpdatePowerAnticipation(cfg, 1.0, 1000u, 120.0), 1.0, kEps,
               "at start_w: rise zone with zero scale holds");
}

void TestDegenerateBandFullStrength() {
    PowerAnticipationConfig cfg = ValidConfig();
    cfg.full_w = cfg.start_w;  // collapsed band
    ExpectNear(UpdatePowerAnticipation(cfg, 0.0, 1000u, 120.0), 4.0, kEps,
               "degenerate band -> full strength at start_w");
}

void TestDecayBelowStart() {
    const PowerAnticipationConfig cfg = ValidConfig();
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, 50.0), 3.0, kEps,
               "below start_w decays at fall rate");
}

void TestDecayOnUnavailable() {
    const PowerAnticipationConfig cfg = ValidConfig();
    // THE deliberate difference from UpdateBoostStage(BelowStart): NaN input
    // decays instead of holding, so an energy fault releases the boost.
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 1000u, kNaN), 3.0, kEps,
               "NaN input decays (fail-safe), not holds");
    ExpectNear(UpdatePowerAnticipation(cfg, 0.0, 1000u, kNaN), 0.0, kEps,
               "NaN input at zero boost stays zero");
}

void TestClampAndCarry() {
    const PowerAnticipationConfig cfg = ValidConfig();
    ExpectNear(UpdatePowerAnticipation(cfg, 9.0, 1000u, 200.0), 10.0, kEps,
               "rise clamps at max_boost_pct");
    ExpectNear(UpdatePowerAnticipation(cfg, 1.0, 1000u, 50.0), 0.0, kEps,
               "decay clamps at zero");
    ExpectNear(UpdatePowerAnticipation(cfg, kNaN, 1000u, 50.0), 0.0, kEps,
               "NaN carried boost re-baselines to 0");
    ExpectNear(UpdatePowerAnticipation(cfg, 25.0, 0u, 200.0), 10.0, kEps,
               "over-max carried boost clamps even at elapsed 0");
}

void TestZeroElapsedHolds() {
    const PowerAnticipationConfig cfg = ValidConfig();
    ExpectNear(UpdatePowerAnticipation(cfg, 5.0, 0u, 200.0), 5.0, kEps,
               "elapsed 0 returns clamped boost unchanged");
}

// Tick-by-tick scenario at the shipped cadence: 250 ms ticks, ~1 s windows.
// Idle floor, load step to above full_w, producer blank, recovery to idle.
void TestScenario_StepLoadAndFault() {
    const PowerAnticipationConfig cfg = ValidConfig();
    WattsInputState input;
    double boost = 0.0;
    std::uint64_t tick = 0u;

    auto run_ticks = [&](int n, std::uint64_t sample_id, double delta_uj,
                         double window_ms) {
        for (int i = 0; i < n; ++i) {
            ++tick;
            const double watts =
                UpdateWattsInput(&input, sample_id, delta_uj, window_ms, 8u);
            boost = UpdatePowerAnticipation(cfg, boost, 250u, watts);
        }
    };

    // Idle: 45 W windows, 8 ticks -> stays 0.
    run_ticks(4, 1u, 45000000.0, 1000.0);
    run_ticks(4, 2u, 45000000.0, 1000.0);
    ExpectNear(boost, 0.0, kEps, "idle floor never accrues");

    // Load step: 220 W (above full_w) -> 4 %/s; cap 10 reached after 2.5 s.
    for (std::uint64_t w = 3u; w <= 6u; ++w) {
        run_ticks(4, w, 220000000.0, 1000.0);
    }
    ExpectNear(boost, 10.0, kEps, "sustained load saturates at max (16 ticks)");

    // Producer blanks (fault): decay at 2 %/s.
    run_ticks(4, 7u, kNaN, 1000.0);
    ExpectNear(boost, 8.0, kEps, "one blanked second decays 2.0");

    // Producer stalls entirely (same sample id forever): after the hold
    // budget the input goes stale-NaN and decay continues to zero.
    run_ticks(40, 7u, kNaN, 1000.0);
    ExpectNear(boost, 0.0, kEps, "stalled producer releases fully");
}

}  // namespace

int main() {
    TestWattsFromWindow();
    TestEma();
    TestWattsInput_NoWindowYet();
    TestWattsInput_NewWindowAndHold();
    TestWattsInput_StaleAgesOut();
    TestWattsInput_BlankWindowPropagates();
    TestGuard_DisabledConfigs();
    TestRiseScale();
    TestDegenerateBandFullStrength();
    TestDecayBelowStart();
    TestDecayOnUnavailable();
    TestClampAndCarry();
    TestZeroElapsedHolds();
    TestScenario_StepLoadAndFault();
    if (g_failures > 0) {
        std::cerr << g_failures << " power_anticipation test failure(s)\n";
        return 1;
    }
    std::cout << "power_anticipation tests OK\n";
    return 0;
}
