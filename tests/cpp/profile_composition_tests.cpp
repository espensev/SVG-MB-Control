// FEAT-0023 (REQ-MPROFILE-01/10) machine-base + behavior-overlay composition.
//
// The reproduction tests read the REAL shipped config tree
// (SVG_MB_CONTROL_REPO_DIR, set by CMake) and prove that composing the snd-desk
// machine base with the release behavior overlay yields exactly the shipped
// control.release.json behavior (the measurement-gate identity + the anti-drift
// pin): if control.release.json changes without the overlay following, these
// fail. The synthesized tests cover the composition logic in isolation: the
// minimum duty is taken from the machine base (not the overlay), and an overlay
// channel that the machine base does not control is rejected.

#include "control_config.h"
#include "control_loop.h"
#include "profile_composition.h"

#include "test_helpers.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

std::filesystem::path RepoConfigDir() {
    return std::filesystem::path(SVG_MB_CONTROL_REPO_DIR) / "config";
}

void ExpectThrowsContaining(const std::function<void()>& thunk,
                            std::string_view needle,
                            const std::string& message) {
    try {
        thunk();
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected throw containing ["
                  << needle << "], but no exception\n";
    } catch (const std::exception& ex) {
        const std::string what(ex.what());
        if (what.find(needle) == std::string::npos) {
            ++g_failures;
            std::cerr << "FAIL: " << message << " expected throw containing ["
                      << needle << "], got [" << what << "]\n";
        }
    }
}

void ExpectCurveEqual(const std::vector<svg_mb_control::CurvePoint>& actual,
                      const std::vector<svg_mb_control::CurvePoint>& expected,
                      const std::string& label) {
    ExpectTrue(actual.size() == expected.size(),
               label + " curve size (" + std::to_string(actual.size()) +
                   " vs " + std::to_string(expected.size()) + ")");
    const std::size_t n = (std::min)(actual.size(), expected.size());
    for (std::size_t i = 0; i < n; ++i) {
        ExpectNear(actual[i].temp_c, expected[i].temp_c, 1e-9,
                   label + " curve[" + std::to_string(i) + "].temp_c");
        ExpectNear(actual[i].duty_pct, expected[i].duty_pct, 1e-9,
                   label + " curve[" + std::to_string(i) + "].duty_pct");
    }
}

// Field-by-field control-identity comparison of two loaded ControlLoopConfigs.
// Covers cadence, low-band, and every per-channel field that enters the control
// computation, so any drift between the composed and shipped configs fails.
void ExpectLoopConfigEqual(const svg_mb_control::ControlLoopConfig& actual,
                           const svg_mb_control::ControlLoopConfig& expected) {
    ExpectTrue(actual.poll_tick_ms == expected.poll_tick_ms, "poll_tick_ms");
    ExpectTrue(actual.write_cooldown_ms == expected.write_cooldown_ms,
               "write_cooldown_ms");
    ExpectNear(actual.deadband_pct, expected.deadband_pct, 1e-9, "deadband_pct");
    ExpectTrue(actual.control_hold_ms == expected.control_hold_ms,
               "control_hold_ms");
    ExpectTrue(actual.low_band.enabled == expected.low_band.enabled,
               "low_band.enabled");
    ExpectNear(actual.low_band.cpu_start_c, expected.low_band.cpu_start_c, 1e-9,
               "low_band.cpu_start_c");
    ExpectNear(actual.low_band_residual_cap_pct,
               expected.low_band_residual_cap_pct, 1e-9,
               "low_band_residual_cap_pct");

    ExpectTrue(actual.channels.size() == expected.channels.size(),
               "channels.size (" + std::to_string(actual.channels.size()) +
                   " vs " + std::to_string(expected.channels.size()) + ")");
    const std::size_t n = (std::min)(actual.channels.size(),
                                     expected.channels.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = actual.channels[i];
        const auto& e = expected.channels[i];
        const std::string p = "channel[" + std::to_string(i) + "]";
        // Order is preserved (overlay-driven) and the injected minimum duty
        // equals the machine base / shipped value.
        ExpectTrue(a.channel == e.channel, p + ".channel");
        ExpectNear(a.min_duty_pct, e.min_duty_pct, 1e-9, p + ".min_duty_pct");
        ExpectNear(a.source_aware_cpu_hot_guard_c,
                   e.source_aware_cpu_hot_guard_c, 1e-9,
                   p + ".source_aware_cpu_hot_guard_c");
        ExpectNear(a.rise_rate_pct_per_min, e.rise_rate_pct_per_min, 1e-9,
                   p + ".rise_rate_pct_per_min");
        ExpectNear(a.fall_rate_pct_per_min, e.fall_rate_pct_per_min, 1e-9,
                   p + ".fall_rate_pct_per_min");
        ExpectNear(a.max_setpoint_step_pct, e.max_setpoint_step_pct, 1e-9,
                   p + ".max_setpoint_step_pct");
        ExpectTrue(a.low_band_stage == e.low_band_stage, p + ".low_band_stage");
        ExpectTrue(static_cast<int>(a.temp_blend) ==
                       static_cast<int>(e.temp_blend),
                   p + ".temp_blend");
        for (std::size_t s = 0; s < svg_mb_control::kBoostStageCount; ++s) {
            const std::string bp = p + ".boost[" + std::to_string(s) + "]";
            ExpectNear(a.boosts[s].start_c, e.boosts[s].start_c, 1e-9,
                       bp + ".start_c");
            ExpectNear(a.boosts[s].full_c, e.boosts[s].full_c, 1e-9,
                       bp + ".full_c");
            ExpectNear(a.boosts[s].max_boost_pct, e.boosts[s].max_boost_pct,
                       1e-9, bp + ".max_boost_pct");
        }
        ExpectCurveEqual(a.curve, e.curve, p);
        ExpectCurveEqual(a.cpu_override_curve, e.cpu_override_curve,
                         p + ".cpu_override");
    }
}

// REQ-MPROFILE-01/10: composing the shipped snd-desk machine base with the
// release behavior overlay reproduces the shipped control.release.json control
// loop exactly (the measurement-gate identity + anti-drift pin).
void TestComposeReproducesReleaseControlLoop() {
    const std::filesystem::path config = RepoConfigDir();
    const auto reference = svg_mb_control::LoadControlLoopConfig(
        config / "control.release.json");
    const auto composed = svg_mb_control::LoadControlLoopConfig(
        config / "profiles" / "snd-desk-composed.json");
    ExpectLoopConfigEqual(composed, reference);
}

// REQ-MPROFILE-10: the full top-level ControlConfig reproduces the shipped
// config, INCLUDING the resolved runtime paths. The overlay's "..\" path
// prefixes resolve relative to config/overlays/ back to config/, so the composed
// profile is a byte-identical drop-in for control.release.json — making an
// eventual machine-identity reroute a verified no-op.
void TestComposeReproducesReleaseTopLevel() {
    const std::filesystem::path config = RepoConfigDir();
    const auto reference =
        svg_mb_control::LoadControlConfig(config / "control.release.json");
    const auto composed = svg_mb_control::LoadControlConfig(
        config / "profiles" / "snd-desk-composed.json");
    ExpectTrue(composed.schema_version == reference.schema_version,
               "schema_version");
    ExpectEqual(composed.default_mode, reference.default_mode, "default_mode");
    ExpectTrue(composed.poll_ms == reference.poll_ms, "poll_ms");
    ExpectTrue(composed.log_rotate_hours == reference.log_rotate_hours,
               "log_rotate_hours");
    ExpectTrue(composed.log_retain_days == reference.log_retain_days,
               "log_retain_days");
    ExpectTrue(composed.csv_flush_interval_rows ==
                   reference.csv_flush_interval_rows,
               "csv_flush_interval_rows");
    ExpectEqual(composed.evidence_gpu_sample_mode,
                reference.evidence_gpu_sample_mode, "evidence_gpu_sample_mode");
    ExpectTrue(composed.baseline_freshness_ceiling_ms ==
                   reference.baseline_freshness_ceiling_ms,
               "baseline_freshness_ceiling_ms");
    ExpectTrue(composed.restore_timeout_ms == reference.restore_timeout_ms,
               "restore_timeout_ms");
    ExpectEqual(composed.runtime_home_path.string(),
                reference.runtime_home_path.string(), "runtime_home_path");
    ExpectEqual(composed.runtime_policy_path.string(),
                reference.runtime_policy_path.string(), "runtime_policy_path");
    ExpectEqual(composed.snapshot_path.string(),
                reference.snapshot_path.string(), "snapshot_path");
}

// A scratch directory of co-located compose inputs, removed on scope exit.
class TempComposeDir {
  public:
    TempComposeDir() {
        std::error_code ec;
        dir_ = std::filesystem::temp_directory_path(ec) /
               (std::string("svg_mb_compose_") + UniqueTempSuffix());
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            throw std::runtime_error("could not create temp compose dir");
        }
    }
    ~TempComposeDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempComposeDir(const TempComposeDir&) = delete;
    TempComposeDir& operator=(const TempComposeDir&) = delete;

    std::filesystem::path Write(const std::string& name,
                                std::string_view content) const {
        const std::filesystem::path path = dir_ / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) {
            throw std::runtime_error("could not write " + path.string());
        }
        return path;
    }

  private:
    std::filesystem::path dir_;
};

// REQ-MPROFILE-01: the minimum duty comes from the machine base, not the
// overlay. The overlay omits min_duty_pct; composition injects the policy's
// release_min_duty_pct.
void TestComposeInjectsMachineBaseMinDuty() {
    TempComposeDir dir;
    dir.Write("machine.json", R"({
      "schema": "svg_mb_control.machine_cooling_policy.v1",
      "fans": [
        {"channel": 0, "direction": "exhaust", "release_min_duty_pct": 11.0},
        {"channel": 6, "direction": "excluded", "release_min_duty_pct": null}
      ]
    })");
    dir.Write("overlay.json", R"({
      "default_mode": "control-loop",
      "control_loop": {
        "poll_tick_ms": 250,
        "channels": [
          {"channel": 0,
           "curve": [{"temp_c": 30, "duty_pct": 20},
                     {"temp_c": 90, "duty_pct": 100}]}
        ]
      }
    })");
    const std::filesystem::path descriptor = dir.Write("profile.json", R"({
      "compose": {"machine_base": "machine.json",
                  "behavior_overlay": "overlay.json"}
    })");

    const auto loaded = svg_mb_control::LoadControlLoopConfig(descriptor);
    ExpectTrue(loaded.channels.size() == 1, "one composed channel");
    if (!loaded.channels.empty()) {
        ExpectNear(loaded.channels[0].min_duty_pct, 11.0, 1e-9,
                   "min_duty injected from machine base");
    }
}

// REQ-MPROFILE-01: an overlay channel that the machine base does not control is
// rejected (the machine base owns the controlled-channel inventory).
void TestComposeRejectsUncontrolledChannel() {
    TempComposeDir dir;
    dir.Write("machine.json", R"({
      "fans": [
        {"channel": 0, "direction": "exhaust", "release_min_duty_pct": 11.0}
      ]
    })");
    dir.Write("overlay.json", R"({
      "control_loop": {
        "channels": [
          {"channel": 9,
           "curve": [{"temp_c": 30, "duty_pct": 20},
                     {"temp_c": 90, "duty_pct": 100}]}
        ]
      }
    })");
    const std::filesystem::path descriptor = dir.Write("profile.json", R"({
      "compose": {"machine_base": "machine.json",
                  "behavior_overlay": "overlay.json"}
    })");

    ExpectThrowsContaining(
        [&] { svg_mb_control::LoadControlLoopConfig(descriptor); },
        "not a controlled channel",
        "overlay channel absent from the machine base is rejected");
}

}  // namespace

int main() {
    TestComposeReproducesReleaseControlLoop();
    TestComposeReproducesReleaseTopLevel();
    TestComposeInjectsMachineBaseMinDuty();
    TestComposeRejectsUncontrolledChannel();

    if (g_failures > 0) {
        std::cerr << g_failures << " profile_composition test failure(s)\n";
        return 1;
    }
    std::cout << "profile_composition tests passed\n";
    return 0;
}
