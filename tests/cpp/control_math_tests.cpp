// Unit tests for the shared math primitives in src/control/control_math.cpp.
// These are pure functions used by cadence_score and low_band_integrator;
// the boost-stage integrator computes equivalent smootherstep math
// internally for its own degenerate-window handling.

#include "control_math.h"

#include "test_helpers.h"

#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr double kEps = 1e-12;

void TestSmoothStep_Endpoints() {
    using svg_mb_control::SmoothStep;
    ExpectNear(SmoothStep(0.0), 0.0, kEps, "SmoothStep(0) == 0");
    ExpectNear(SmoothStep(1.0), 1.0, kEps, "SmoothStep(1) == 1");
    // Quintic smootherstep is symmetric: S(0.5) = 0.5 exactly.
    ExpectNear(SmoothStep(0.5), 0.5, kEps, "SmoothStep(0.5) == 0.5");
}

void TestSmoothStep_Clamps() {
    using svg_mb_control::SmoothStep;
    ExpectNear(SmoothStep(-0.1), 0.0, kEps, "SmoothStep(-0.1) clamps to 0");
    ExpectNear(SmoothStep(-100.0), 0.0, kEps, "SmoothStep(-100) clamps to 0");
    ExpectNear(SmoothStep(1.1), 1.0, kEps, "SmoothStep(1.1) clamps to 1");
    ExpectNear(SmoothStep(1000.0), 1.0, kEps, "SmoothStep(1000) clamps to 1");
}

void TestSmoothStep_Monotonic() {
    using svg_mb_control::SmoothStep;
    double previous = SmoothStep(0.0);
    for (int i = 1; i <= 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        const double current = SmoothStep(t);
        ExpectTrue(current >= previous,
                   "SmoothStep monotonic non-decreasing at t=" +
                       std::to_string(t));
        previous = current;
    }
    // Inflection around 0.5 means the derivative is positive but the curve
    // is bounded. Bound check is enough; exact derivative values aren't
    // part of the contract.
}

void TestSmoothScale_BelowStart() {
    using svg_mb_control::SmoothScale;
    ExpectNear(SmoothScale(0.0, 50.0, 100.0), 0.0, kEps,
               "SmoothScale below start clamps to 0");
    ExpectNear(SmoothScale(49.999, 50.0, 100.0), 0.0, kEps,
               "SmoothScale just below start clamps to 0");
}

void TestSmoothScale_AboveFull() {
    using svg_mb_control::SmoothScale;
    ExpectNear(SmoothScale(100.0, 50.0, 100.0), 1.0, kEps,
               "SmoothScale at full clamps to 1");
    ExpectNear(SmoothScale(150.0, 50.0, 100.0), 1.0, kEps,
               "SmoothScale above full clamps to 1");
}

void TestSmoothScale_Midband() {
    using svg_mb_control::SmoothScale;
    // value=75, start=50, full=100 → t=0.5 → S(0.5) = 0.5.
    ExpectNear(SmoothScale(75.0, 50.0, 100.0), 0.5, kEps,
               "SmoothScale midband == 0.5");
}

void TestSmoothScale_NaN() {
    using svg_mb_control::SmoothScale;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    ExpectNear(SmoothScale(nan, 50.0, 100.0), 0.0, kEps,
               "SmoothScale NaN value returns 0");
}

void TestSmoothScale_DegenerateBand() {
    using svg_mb_control::SmoothScale;
    // full == start: SmoothScale falls through to the inert path.
    ExpectNear(SmoothScale(50.0, 50.0, 50.0), 0.0, kEps,
               "SmoothScale full==start returns 0");
    // full < start (inverted): same path.
    ExpectNear(SmoothScale(75.0, 100.0, 50.0), 0.0, kEps,
               "SmoothScale inverted band returns 0");
}

void TestMoveToward_SnapsOnSmallDelta() {
    using svg_mb_control::MoveTowardRateLimited;
    // |delta| <= 1e-4 short-circuits to target regardless of rate.
    ExpectNear(MoveTowardRateLimited(5.0, 5.00005, 1.0, 100.0, 100.0),
               5.00005, kEps,
               "MoveToward sub-epsilon delta snaps to target");
}

void TestMoveToward_NaNCurrentTreatedAsZero() {
    using svg_mb_control::MoveTowardRateLimited;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // NaN current → 0.0; target=2, dt=1min, rise=10/min → allowed=10,
    // |delta|=2 <= 10 → snap to target.
    ExpectNear(MoveTowardRateLimited(nan, 2.0, 1.0, 10.0, 10.0),
               2.0, kEps,
               "MoveToward NaN current converges via zero baseline");
    // NaN current with target far away → rate-limit from 0.
    // current=0, target=100, dt=1min, rise=10 → allowed=10, |delta|=100,
    // → return 0 + 10 = 10.
    ExpectNear(MoveTowardRateLimited(nan, 100.0, 1.0, 10.0, 10.0),
               10.0, kEps,
               "MoveToward NaN current rate-limits from 0 baseline");
}

void TestMoveToward_SnapOnNonPositiveRate() {
    using svg_mb_control::MoveTowardRateLimited;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // rate NaN, 0, or negative → snap to target (no rate limit applied).
    ExpectNear(MoveTowardRateLimited(0.0, 100.0, 1.0, nan, 10.0),
               100.0, kEps,
               "MoveToward NaN rise rate snaps rising move");
    ExpectNear(MoveTowardRateLimited(0.0, 100.0, 1.0, 0.0, 10.0),
               100.0, kEps,
               "MoveToward zero rise rate snaps rising move");
    ExpectNear(MoveTowardRateLimited(100.0, 0.0, 1.0, 10.0, -5.0),
               0.0, kEps,
               "MoveToward negative fall rate snaps falling move");
}

void TestMoveToward_SnapOnNonPositiveDt() {
    using svg_mb_control::MoveTowardRateLimited;
    // dt_minutes == 0 or < 0 → snap to target.
    ExpectNear(MoveTowardRateLimited(0.0, 100.0, 0.0, 10.0, 10.0),
               100.0, kEps,
               "MoveToward zero dt snaps to target");
    ExpectNear(MoveTowardRateLimited(0.0, 100.0, -1.0, 10.0, 10.0),
               100.0, kEps,
               "MoveToward negative dt snaps to target");
}

void TestMoveToward_RateLimitedRise() {
    using svg_mb_control::MoveTowardRateLimited;
    // current=10, target=20, dt=1min, rise=5/min, fall=5/min →
    // allowed=5, delta=10 > allowed, result = 10 + 5 = 15.
    ExpectNear(MoveTowardRateLimited(10.0, 20.0, 1.0, 5.0, 5.0),
               15.0, kEps,
               "MoveToward rate-limited rise = current + rise*dt");
}

void TestMoveToward_RateLimitedFall() {
    using svg_mb_control::MoveTowardRateLimited;
    // current=20, target=10, dt=1min, rise=10, fall=2 → allowed=2,
    // |delta|=10 > 2, result = 20 - 2 = 18 (fall direction uses fall rate).
    ExpectNear(MoveTowardRateLimited(20.0, 10.0, 1.0, 10.0, 2.0),
               18.0, kEps,
               "MoveToward rate-limited fall = current - fall*dt");
}

void TestMoveToward_AsymmetricRates() {
    using svg_mb_control::MoveTowardRateLimited;
    // Confirms rise uses rise_per_min and fall uses fall_per_min by
    // running both directions with very different rates from the same
    // starting point.
    const double after_rise =
        MoveTowardRateLimited(50.0, 60.0, 1.0, 4.0, 100.0);
    ExpectNear(after_rise, 54.0, kEps,
               "MoveToward asymmetric: rise uses rise_per_min");
    const double after_fall =
        MoveTowardRateLimited(50.0, 40.0, 1.0, 100.0, 4.0);
    ExpectNear(after_fall, 46.0, kEps,
               "MoveToward asymmetric: fall uses fall_per_min");
}

void TestMoveToward_AllowedExceedsDelta() {
    using svg_mb_control::MoveTowardRateLimited;
    // |delta| <= allowed → snap to target without rate-limit-step math.
    // current=10, target=11, dt=1min, rise=100 → allowed=100 >> 1, snap.
    ExpectNear(MoveTowardRateLimited(10.0, 11.0, 1.0, 100.0, 100.0),
               11.0, kEps,
               "MoveToward delta <= allowed snaps to target");
}

}  // namespace

int main() {
    TestSmoothStep_Endpoints();
    TestSmoothStep_Clamps();
    TestSmoothStep_Monotonic();
    TestSmoothScale_BelowStart();
    TestSmoothScale_AboveFull();
    TestSmoothScale_Midband();
    TestSmoothScale_NaN();
    TestSmoothScale_DegenerateBand();
    TestMoveToward_SnapsOnSmallDelta();
    TestMoveToward_NaNCurrentTreatedAsZero();
    TestMoveToward_SnapOnNonPositiveRate();
    TestMoveToward_SnapOnNonPositiveDt();
    TestMoveToward_RateLimitedRise();
    TestMoveToward_RateLimitedFall();
    TestMoveToward_AsymmetricRates();
    TestMoveToward_AllowedExceedsDelta();
    if (g_failures > 0) {
        std::cerr << g_failures << " control_math test failure(s)\n";
        return 1;
    }
    std::cout << "control_math tests OK\n";
    return 0;
}
