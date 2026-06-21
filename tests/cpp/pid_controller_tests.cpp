// Tests for the FEAT-0003 PID control law (slice F3-3). The core is PidStep,
// the pure per-step math (REQ-PROFILE-02): P/PI/PD/PID by gain selection,
// derivative-on-measurement with the POSITIVE sign for the error = temp - target
// cooling convention, and clamp + conditional-integration anti-windup. The
// derivative-sign and anti-windup tests encode the physical expectation directly
// (rising temp must raise duty; a saturated integral must not wind up) so they
// catch a wrong sign or missing anti-windup regardless of the implementation.
// PidController::Evaluate is covered by a wiring smoke; the live/shadow gate
// (decision D6) is covered by PidLiveAuthorized cases.

#include "pid_controller.h"

#include "test_helpers.h"

#include "channel_controller.h"
#include "channel_evaluator.h"
#include "control_loop.h"
#include "control_runtime_context.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

using namespace svg_mb_control;

// --- PidStep: pure math ----------------------------------------------------

// Pure P: error = temp - target, output = bias + Kp*error, no I/D term.
void TestPureProportional() {
    PidState state;
    const PidGains gains{2.0, 0.0, 0.0};
    const PidTerms t = PidStep(gains, /*bias*/ 0.0, /*temp*/ 60.0,
                               /*target*/ 50.0, /*dt*/ 1.0, 0.0, 100.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), state);
    ExpectNear(t.error_c, 10.0, 1e-12, "pure-P error = temp - target");
    ExpectNear(t.p_term, 20.0, 1e-12, "pure-P p_term = Kp*error");
    ExpectNear(t.i_term, 0.0, 1e-12, "pure-P has no integral term");
    ExpectNear(t.d_term, 0.0, 1e-12, "pure-P has no derivative term");
    ExpectNear(t.raw_setpoint_pct, 20.0, 1e-12, "pure-P raw = bias + p");
}

// The advisor's sign pin: with Kp=Ki=0, Kd>0 and a RISING temperature, the
// setpoint must go UP (anticipatory cooling). A textbook -Kd*d(temp)/dt sign
// would make it go DOWN and fight the P term -- this test fails in that case.
void TestDerivativeSignRaisesDutyOnRisingTemp() {
    PidState state;
    const PidGains gains{0.0, 0.0, 1.0};
    const PidTerms first = PidStep(gains, /*bias*/ 30.0, /*temp*/ 55.0,
                                   /*target*/ 50.0, /*dt*/ 1.0, 0.0, 100.0,
                                   std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::quiet_NaN(),
                                   state);
    ExpectNear(first.d_term, 0.0, 1e-12, "no derivative on the first step");
    ExpectNear(first.raw_setpoint_pct, 30.0, 1e-12, "first raw = bias only");

    const PidTerms second = PidStep(gains, /*bias*/ 30.0, /*temp*/ 60.0,
                                    /*target*/ 50.0, /*dt*/ 1.0, 0.0, 100.0,
                                    std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::quiet_NaN(),
                                    state);
    ExpectNear(second.d_term, 5.0, 1e-12,
               "d_term = +Kd*(temp - prev_temp)/dt = 1*(60-55)/1");
    ExpectTrue(second.raw_setpoint_pct > first.raw_setpoint_pct,
               "rising temperature RAISES the setpoint (anticipatory cooling)");
    ExpectNear(second.raw_setpoint_pct, 35.0, 1e-12, "second raw = bias + d");
}

// Derivative-on-measurement: changing target_c produces no derivative kick,
// because the derivative is taken on the measured temperature, not the error.
void TestDerivativeOnMeasurementNoKickOnTargetChange() {
    PidState state;
    const PidGains gains{0.0, 0.0, 1.0};
    PidStep(gains, 0.0, /*temp*/ 50.0, /*target*/ 50.0, 1.0, 0.0, 100.0,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(), state);
    // Same measurement, but target drops 10C: derivative-on-error would spike;
    // derivative-on-measurement stays zero.
    const PidTerms t = PidStep(gains, 0.0, /*temp*/ 50.0, /*target*/ 40.0, 1.0,
                               0.0, 100.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), state);
    ExpectNear(t.d_term, 0.0, 1e-12,
               "no derivative kick when only target_c changes");
}

// PI: the integral accumulates error*dt across steps (no integration on step 1).
void TestIntegralAccumulates() {
    PidState state;
    const PidGains gains{0.0, 0.5, 0.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const PidTerms s1 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectNear(s1.i_term, 0.0, 1e-12, "no integration on the first step");
    const PidTerms s2 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectNear(s2.i_term, 5.0, 1e-12, "i_term = Ki*(error*dt) = 0.5*10");
    const PidTerms s3 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectNear(s3.i_term, 10.0, 1e-12, "i_term accumulates: 0.5*(10+10)");
}

// Conditional-integration anti-windup: while the output is saturated against the
// high bound and the error keeps pushing up, the integral must NOT grow.
void TestAntiWindupFreezesAtSaturation() {
    PidState state;
    const PidGains gains{0.0, 1.0, 0.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // bias 95 + integral would exceed 100 immediately on the first integrating
    // step; error is positive (60 > 50), so integration is frozen.
    PidStep(gains, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);  // step1: no integ
    const PidTerms s2 =
        PidStep(gains, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectNear(s2.i_term, 0.0, 1e-12, "integral frozen while saturated high");
    const PidTerms s3 =
        PidStep(gains, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectNear(s3.i_term, 0.0, 1e-12, "integral still frozen (no windup)");
}

// Integral clamp bound caps the accumulator even when not output-saturated.
void TestIntegralClampBound() {
    PidState state;
    const PidGains gains{0.0, 1.0, 0.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, 15.0, state);  // step1
    const PidTerms s2 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, 15.0, state);
    ExpectNear(s2.i_term, 10.0, 1e-12, "integral 10 < clamp 15");
    const PidTerms s3 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, 15.0, state);
    ExpectNear(s3.i_term, 15.0, 1e-12, "integral clamped to 15");
    const PidTerms s4 =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, 15.0, state);
    ExpectNear(s4.i_term, 15.0, 1e-12, "integral stays clamped at 15");
}

// The D6 live gate accepts a step cap alone (no rise/fall rates) as a sufficient
// slew bound, so RateLimitSetpoint MUST honor max_setpoint_step_pct even when the
// directional rate is NaN -- otherwise a live step-cap-only PID has no per-tick
// limit (D4/D6 bypass). The rate-present behavior must be unchanged.
void TestRateLimit_StepCapAppliesEvenWithoutRates() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    ExpectNear(RateLimitSetpoint(90.0, 30.0, 1000u, nan, nan, 5.0), 35.0, 1e-12,
               "step cap clamps the rise when rates are NaN");
    ExpectNear(RateLimitSetpoint(0.0, 30.0, 1000u, nan, nan, 5.0), 25.0, 1e-12,
               "step cap clamps the fall when rates are NaN");
    ExpectNear(RateLimitSetpoint(90.0, 30.0, 1000u, nan, nan, nan), 90.0, 1e-12,
               "no rate and no step cap -> no limit (unchanged)");
    ExpectNear(RateLimitSetpoint(90.0, nan, 1000u, nan, nan, 5.0), 90.0, 1e-12,
               "first write (last NaN) is unlimited (unchanged)");
    ExpectNear(RateLimitSetpoint(90.0, 30.0, 60000u, 30.0, 30.0, 5.0), 35.0,
               1e-12, "rate+step both present -> min applies (unchanged)");
    ExpectNear(RateLimitSetpoint(90.0, 30.0, 60000u, 6.0, 6.0, nan), 36.0, 1e-12,
               "rate-only limit unchanged when no step cap");
}

// A NaN measurement (sensor glitch with the source still flagged available) must
// not permanently poison the persistent integral accumulator.
void TestPidStep_NaNMeasurementDoesNotPoisonIntegral() {
    PidState state;
    const PidGains gains{1.0, 1.0, 1.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectTrue(std::isfinite(state.integral), "healthy integral established");
    PidStep(gains, 0.0, nan, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectTrue(std::isfinite(state.integral),
               "NaN measurement does not corrupt the integral");
    const PidTerms good =
        PidStep(gains, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, nan, nan, state);
    ExpectTrue(std::isfinite(good.raw_setpoint_pct),
               "PID recovers to a finite output after a NaN measurement");
}

// --- PidLiveAuthorized: the decision-D6 gate -------------------------------

ChannelControlConfig BasicPidConfig() {
    ChannelControlConfig config;
    config.controller_kind = ControllerKind::Pid;
    config.pid.target_c = 65.0;
    config.pid.kp = 1.0;
    return config;
}

void TestLiveGate_AllowLiveFalseIsShadow() {
    ChannelControlConfig config = BasicPidConfig();
    config.pid.allow_live = false;
    std::string reason;
    ExpectTrue(!PidLiveAuthorized(config, reason),
               "allow_live false -> not live");
    ExpectTrue(reason.find("allow_live") != std::string::npos,
               "shadow reason names allow_live");
}

void TestLiveGate_NoSlewCapIsShadow() {
    ChannelControlConfig config = BasicPidConfig();
    config.pid.allow_live = true;
    config.pid.characterization_artifact = "anything";
    std::string reason;
    ExpectTrue(!PidLiveAuthorized(config, reason),
               "allow_live without slew cap -> not live");
    ExpectTrue(reason.find("slew") != std::string::npos,
               "shadow reason names the slew cap");
}

void TestLiveGate_MissingArtifactIsShadow() {
    ChannelControlConfig config = BasicPidConfig();
    config.pid.allow_live = true;
    config.max_setpoint_step_pct = 5.0;  // positive slew cap present
    config.pid.characterization_artifact =
        "definitely-not-a-real-file-xyzzy.csv";
    std::string reason;
    ExpectTrue(!PidLiveAuthorized(config, reason),
               "allow_live + slew but missing artifact -> not live");
    ExpectTrue(reason.find("not found") != std::string::npos,
               "shadow reason names the missing artifact");
}

void TestLiveGate_AllPreconditionsMetIsLive() {
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec) /
                     (std::string("svg_mb_pid_artifact_") + UniqueTempSuffix() +
                      ".csv");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "shadow-log evidence";
    }
    ChannelControlConfig config = BasicPidConfig();
    config.pid.allow_live = true;
    config.max_setpoint_step_pct = 5.0;
    config.pid.characterization_artifact = tmp.string();
    std::string reason;
    const bool live = PidLiveAuthorized(config, reason);
    std::filesystem::remove(tmp, ec);
    ExpectTrue(live, "allow_live + slew cap + existing artifact -> live");
    ExpectTrue(reason.empty(), "live -> empty shadow reason");
}

// --- PidController::Evaluate wiring ----------------------------------------

void TestController_ShadowByDefaultAndKind() {
    ControlLoopConfig loop;
    ChannelControlConfig cfg = BasicPidConfig();
    cfg.channel = 0u;
    cfg.temp_blend = TempBlend::CpuOnly;
    cfg.curve = {{30.0, 20.0}, {70.0, 100.0}};
    cfg.pid.kp = 2.0;
    cfg.pid.feedforward = PidFeedforward::Fixed;
    cfg.pid.fixed_feedforward_pct = 0.0;

    ChannelState channel;
    channel.config = cfg;

    PidController controller(cfg);
    ExpectEqual(std::string(controller.Kind()), "pid",
                "PidController Kind() is pid");
    ExpectTrue(!controller.live(), "default pid (no allow_live) is shadow");

    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    TempInputs in;
    in.cpu_c = 75.0;  // target 65 -> error +10, Kp 2 -> +20 over fixed bias 0
    in.cpu_available = true;
    in.cpu_label = "cpu";

    const ChannelEvaluation eval = controller.Evaluate(
        channel, loop, in, index, std::chrono::steady_clock::now());
    ExpectTrue(eval.has_setpoint, "pid evaluation produces a setpoint");
    ExpectTrue(eval.write_suppressed,
               "shadow pid suppresses the write (REQ-PROFILE-07)");
    ExpectEqual(eval.response_source, std::string("pid"),
                "pid evaluation tags response_source=pid");
    ExpectNear(eval.setpoint_pct, 20.0, 1e-9,
               "shadow pid still computes setpoint = bias 0 + Kp*error 20");
    ExpectNear(channel.pid_error_c, 10.0, 1e-9,
               "pid error recorded on ChannelState for reporting");
    ExpectTrue(std::isnan(channel.last_raw_demand_pct),
               "pid leaves curve-only feedforward (last_raw_demand_pct) NaN");
}

void TestController_SensorUnavailableSafeMode() {
    ControlLoopConfig loop;
    ChannelControlConfig cfg = BasicPidConfig();
    cfg.temp_blend = TempBlend::CpuOnly;
    cfg.curve = {{30.0, 20.0}, {70.0, 100.0}};

    ChannelState channel;
    channel.config = cfg;
    PidController controller(cfg);

    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    TempInputs in;  // cpu_available = false -> primary unavailable
    const ChannelEvaluation eval = controller.Evaluate(
        channel, loop, in, index, std::chrono::steady_clock::now());
    ExpectTrue(eval.has_setpoint, "safe-mode still issues a setpoint");
    ExpectTrue(eval.safety_override,
               "sensor-unavailable PID asserts safety_override");
    ExpectNear(eval.setpoint_pct, ChannelState::kSafeModeFanDuty, 1e-12,
               "safe-mode drives the safe fan duty");
    ExpectEqual(eval.response_source, std::string("sensor_safe_mode"),
                "safe-mode response_source");
}

void TestController_LiveStepCapLimitsLargeJump() {
    // A live step-cap-only PID (no rise/fall rates) must still be slew-limited per
    // tick (D4/D6): a large PID jump from the last issued duty is clamped to
    // max_setpoint_step_pct.
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec) /
                     (std::string("svg_mb_pid_live_") + UniqueTempSuffix() +
                      ".csv");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "shadow-log evidence";
    }
    ControlLoopConfig loop;
    ChannelControlConfig cfg = BasicPidConfig();
    cfg.temp_blend = TempBlend::CpuOnly;
    cfg.curve = {{30.0, 20.0}, {70.0, 100.0}};
    cfg.min_duty_pct = 0.0;
    cfg.pid.kp = 10.0;  // large gain -> large unclamped jump
    cfg.pid.feedforward = PidFeedforward::Fixed;
    cfg.pid.fixed_feedforward_pct = 30.0;
    cfg.pid.allow_live = true;
    cfg.pid.characterization_artifact = tmp.string();
    cfg.max_setpoint_step_pct = 5.0;  // step cap only; rise/fall rates left NaN

    PidController controller(cfg);
    const bool live = controller.live();

    ChannelState channel;
    channel.config = cfg;
    channel.last_issued_pct = 30.0;  // a prior write, so the slew cap engages

    RuntimeSnapshot snap;
    RuntimeSnapshotIndex index;
    index.Rebuild(snap);
    TempInputs in;
    in.cpu_c = 80.0;  // error +15, kp 10 -> +150 over bias 30 -> clamps to 100
    in.cpu_available = true;
    in.cpu_label = "cpu";
    const ChannelEvaluation eval = controller.Evaluate(
        channel, loop, in, index, std::chrono::steady_clock::now());
    std::filesystem::remove(tmp, ec);

    ExpectTrue(live, "step-cap + artifact authorizes a live PID");
    ExpectTrue(!eval.write_suppressed, "a live PID does not suppress the write");
    ExpectNear(eval.setpoint_pct, 35.0, 1e-9,
               "the per-tick jump from 30 is clamped to the 5pct step cap");
}

}  // namespace

int main() {
    TestPureProportional();
    TestRateLimit_StepCapAppliesEvenWithoutRates();
    TestPidStep_NaNMeasurementDoesNotPoisonIntegral();
    TestController_LiveStepCapLimitsLargeJump();
    TestDerivativeSignRaisesDutyOnRisingTemp();
    TestDerivativeOnMeasurementNoKickOnTargetChange();
    TestIntegralAccumulates();
    TestAntiWindupFreezesAtSaturation();
    TestIntegralClampBound();
    TestLiveGate_AllowLiveFalseIsShadow();
    TestLiveGate_NoSlewCapIsShadow();
    TestLiveGate_MissingArtifactIsShadow();
    TestLiveGate_AllPreconditionsMetIsLive();
    TestController_ShadowByDefaultAndKind();
    TestController_SensorUnavailableSafeMode();

    if (g_failures != 0) {
        std::cerr << g_failures << " pid controller test failure(s)\n";
        return 1;
    }
    std::cout << "All pid controller tests passed\n";
    return 0;
}
