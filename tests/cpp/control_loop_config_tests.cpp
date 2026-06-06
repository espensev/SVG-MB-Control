// Integration tests for the boost-stage configuration path through
// LoadControlLoopConfig. Each test writes a minimal JSON config to a
// temporary file, calls the public loader, and asserts either a
// well-formed ControlLoopConfig.boosts[] entry or a thrown
// std::runtime_error containing the expected diagnostic substring.
//
// Covers both release-mode validators:
//   - BelowStart (thermal_pressure, midband_pressure, gpu_airflow):
//     start_c + full_c are required; rise/fall/max may stay NaN; the
//     full_c > start_c invariant is checked.
//   - ExplicitRelease (cpu_low_soak): the complete six-field set is
//     required; full_c > start_c, release_c <= start_c, and
//     max_boost_pct <= 10 invariants are checked.

#include "boost_stage.h"
#include "control_loop.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

int g_failures = 0;

void ExpectTrue(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectFalse(bool condition, const std::string& message) {
    ExpectTrue(!condition, message);
}

void ExpectNear(double actual, double expected, double tolerance,
                const std::string& message) {
    if (std::isnan(actual) && std::isnan(expected)) return;
    if (std::fabs(actual - expected) > tolerance) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected " << expected
                  << " got " << actual << '\n';
    }
}

void ExpectThrowsContaining(const std::function<void()>& thunk,
                            std::string_view needle,
                            const std::string& message) {
    try {
        thunk();
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected throw containing [" << needle
                  << "], but no exception\n";
    } catch (const std::exception& ex) {
        const std::string what(ex.what());
        if (what.find(needle) == std::string::npos) {
            ++g_failures;
            std::cerr << "FAIL: " << message << " expected throw containing ["
                      << needle << "], got [" << what << "]\n";
        }
    }
}

// A per-process-unique filename component so two concurrent test processes
// (for example two Test-LocalCI runs) never share a %TEMP% path. A per-process
// counter alone is insufficient: it resets to the same value in every process,
// so every process would reuse svg_mb_control_config_test_1.json and one run
// could truncate the file mid-read in another. random_device yields a distinct
// salt per process; the counter disambiguates files within one process.
std::string UniqueTempSuffix() {
    static const unsigned long long kProcessSalt = [] {
        std::random_device rd;
        return (static_cast<unsigned long long>(rd()) << 32) ^
               static_cast<unsigned long long>(rd());
    }();
    static unsigned long long counter = 0;
    return std::to_string(kProcessSalt) + "_" + std::to_string(++counter);
}

// Writes the given JSON text to a temp file and removes the file on scope
// exit. The filename carries a per-process salt + counter (UniqueTempSuffix)
// so concurrent test processes get distinct files.
class TempJsonFile {
  public:
    explicit TempJsonFile(std::string_view content) {
        std::error_code ec;
        const auto tmp_dir = std::filesystem::temp_directory_path(ec);
        if (ec || tmp_dir.empty()) {
            throw std::runtime_error(
                "temp_directory_path failed in TempJsonFile setup");
        }
        path_ = tmp_dir / (std::string("svg_mb_control_config_test_") +
                           UniqueTempSuffix() + ".json");
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) {
            throw std::runtime_error(
                "failed to write TempJsonFile at " + path_.string());
        }
    }

    ~TempJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempJsonFile(const TempJsonFile&) = delete;
    TempJsonFile& operator=(const TempJsonFile&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// Builds a minimal control_loop config wrapper around a single channel
// snippet. Callers append per-stage boost fields inside `channel_extra`.
std::string ChannelConfigJson(std::string_view channel_extra) {
    std::string out;
    out.reserve(channel_extra.size() + 256);
    out += R"({"control_loop":{"channels":[{"channel":0,)";
    out += R"("curve":[{"temp_c":30,"duty_pct":20},)";
    out += R"({"temp_c":90,"duty_pct":100}])";
    out += channel_extra;
    out += R"(}]}})";
    return out;
}

// Helper for the BoostStage index, since report-stage indexing repeats.
std::size_t Idx(svg_mb_control::BoostStage stage) {
    return static_cast<std::size_t>(stage);
}

void TestNoBoostFields_AllStagesInactive() {
    // No per-stage fields anywhere; every stage's config is default-NaN.
    TempJsonFile cfg_file(ChannelConfigJson(""));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    ExpectTrue(loaded.channels.size() == 1,
               "single channel loaded with no boosts");

    for (std::size_t i = 0; i < svg_mb_control::kBoostStageCount; ++i) {
        const auto& b = loaded.channels[0].boosts[i];
        ExpectTrue(std::isnan(b.start_c),
                   "stage " + std::to_string(i) + " start_c NaN");
        ExpectTrue(std::isnan(b.full_c),
                   "stage " + std::to_string(i) + " full_c NaN");
        ExpectTrue(std::isnan(b.max_boost_pct),
                   "stage " + std::to_string(i) + " max_boost_pct NaN");
    }
}

void TestThermalPressure_CompleteFields() {
    const std::string body = R"(,"thermal_pressure_start_c":84.0,)"
                             R"("thermal_pressure_full_c":95.0,)"
                             R"("thermal_pressure_rise_pct_per_sec":0.04,)"
                             R"("thermal_pressure_fall_pct_per_sec":0.02,)"
                             R"("thermal_pressure_max_boost_pct":2.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& b = loaded.channels[0].boosts[
        Idx(svg_mb_control::BoostStage::ThermalPressure)];
    ExpectNear(b.start_c, 84.0, 1e-12, "thermal_pressure start_c");
    ExpectNear(b.full_c, 95.0, 1e-12, "thermal_pressure full_c");
    ExpectNear(b.rise_per_unit, 0.04, 1e-12, "thermal_pressure rise");
    ExpectNear(b.fall_per_unit, 0.02, 1e-12, "thermal_pressure fall");
    ExpectNear(b.max_boost_pct, 2.0, 1e-12, "thermal_pressure max_boost_pct");
    ExpectTrue(std::isnan(b.release_c),
               "thermal_pressure release_c stays NaN (BelowStart)");
}

void TestThermalPressure_PartialOk() {
    // BelowStart is lenient: start + full are sufficient, rise/fall/max
    // can stay NaN. UpdateBoostStage treats the spec as inert at runtime.
    const std::string body = R"(,"thermal_pressure_start_c":84.0,)"
                             R"("thermal_pressure_full_c":95.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& b = loaded.channels[0].boosts[
        Idx(svg_mb_control::BoostStage::ThermalPressure)];
    ExpectNear(b.start_c, 84.0, 1e-12, "partial: start_c set");
    ExpectNear(b.full_c, 95.0, 1e-12, "partial: full_c set");
    ExpectTrue(std::isnan(b.rise_per_unit), "partial: rise stays NaN");
    ExpectTrue(std::isnan(b.fall_per_unit), "partial: fall stays NaN");
    ExpectTrue(std::isnan(b.max_boost_pct), "partial: max stays NaN");
}

void TestThermalPressure_MissingStartFails() {
    const std::string body = R"(,"thermal_pressure_full_c":95.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "thermal_pressure requires both start_c and full_c",
        "thermal_pressure full_c without start_c -> throw");
}

void TestThermalPressure_FullEqStartFails() {
    const std::string body = R"(,"thermal_pressure_start_c":84.0,)"
                             R"("thermal_pressure_full_c":84.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "thermal_pressure_full_c must be > start_c",
        "thermal_pressure full_c == start_c -> throw");
}

void TestMidbandPressure_RoundTripsThroughLoader() {
    const std::string body = R"(,"midband_pressure_start_c":64.0,)"
                             R"("midband_pressure_full_c":82.0,)"
                             R"("midband_pressure_rise_pct_per_sec":0.03,)"
                             R"("midband_pressure_fall_pct_per_sec":0.015,)"
                             R"("midband_pressure_max_boost_pct":1.5)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& b = loaded.channels[0].boosts[
        Idx(svg_mb_control::BoostStage::MidbandPressure)];
    ExpectNear(b.start_c, 64.0, 1e-12, "midband start_c");
    ExpectNear(b.full_c, 82.0, 1e-12, "midband full_c");
    ExpectNear(b.rise_per_unit, 0.03, 1e-12, "midband rise");
    ExpectNear(b.max_boost_pct, 1.5, 1e-12, "midband max_boost_pct");
}

void TestGpuAirflow_RoundTripsThroughLoader() {
    const std::string body = R"(,"gpu_airflow_start_c":58.0,)"
                             R"("gpu_airflow_full_c":72.0,)"
                             R"("gpu_airflow_rise_pct_per_sec":0.02,)"
                             R"("gpu_airflow_fall_pct_per_sec":0.01,)"
                             R"("gpu_airflow_max_boost_pct":1.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& b = loaded.channels[0].boosts[
        Idx(svg_mb_control::BoostStage::GpuAirflow)];
    ExpectNear(b.start_c, 58.0, 1e-12, "gpu_airflow start_c");
    ExpectNear(b.full_c, 72.0, 1e-12, "gpu_airflow full_c");
    ExpectNear(b.rise_per_unit, 0.02, 1e-12, "gpu_airflow rise");
}

void TestCpuLowSoak_CompleteFields() {
    const std::string body = R"(,"cpu_low_soak_start_c":72.0,)"
                             R"("cpu_low_soak_full_c":82.0,)"
                             R"("cpu_low_soak_release_c":68.0,)"
                             R"("cpu_low_soak_rise_pct_per_min":0.3,)"
                             R"("cpu_low_soak_fall_pct_per_min":0.3,)"
                             R"("cpu_low_soak_max_boost_pct":0.3)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& b = loaded.channels[0].boosts[
        Idx(svg_mb_control::BoostStage::CpuLowSoak)];
    ExpectNear(b.start_c, 72.0, 1e-12, "cpu_low_soak start_c");
    ExpectNear(b.full_c, 82.0, 1e-12, "cpu_low_soak full_c");
    ExpectNear(b.release_c, 68.0, 1e-12, "cpu_low_soak release_c");
    ExpectNear(b.rise_per_unit, 0.3, 1e-12, "cpu_low_soak rise per_min");
    ExpectNear(b.fall_per_unit, 0.3, 1e-12, "cpu_low_soak fall per_min");
    ExpectNear(b.max_boost_pct, 0.3, 1e-12, "cpu_low_soak max_boost_pct");
}

void TestCpuLowSoak_PartialFails() {
    // ExplicitRelease is strict: any field set requires the full set.
    const std::string body = R"(,"cpu_low_soak_start_c":72.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "cpu_low_soak requires the complete field set",
        "cpu_low_soak partial fields -> throw");
}

void TestCpuLowSoak_ReleaseGtStartFails() {
    const std::string body = R"(,"cpu_low_soak_start_c":72.0,)"
                             R"("cpu_low_soak_full_c":82.0,)"
                             R"("cpu_low_soak_release_c":75.0,)"
                             R"("cpu_low_soak_rise_pct_per_min":0.3,)"
                             R"("cpu_low_soak_fall_pct_per_min":0.3,)"
                             R"("cpu_low_soak_max_boost_pct":0.3)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "cpu_low_soak_release_c must be <= start_c",
        "cpu_low_soak release_c > start_c -> throw");
}

void TestCpuLowSoak_FullEqStartFails() {
    const std::string body = R"(,"cpu_low_soak_start_c":72.0,)"
                             R"("cpu_low_soak_full_c":72.0,)"
                             R"("cpu_low_soak_release_c":68.0,)"
                             R"("cpu_low_soak_rise_pct_per_min":0.3,)"
                             R"("cpu_low_soak_fall_pct_per_min":0.3,)"
                             R"("cpu_low_soak_max_boost_pct":0.3)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "cpu_low_soak_full_c must be > start_c",
        "cpu_low_soak full_c == start_c -> throw");
}

void TestCpuLowSoak_MaxBoostOver10Fails() {
    const std::string body = R"(,"cpu_low_soak_start_c":72.0,)"
                             R"("cpu_low_soak_full_c":82.0,)"
                             R"("cpu_low_soak_release_c":68.0,)"
                             R"("cpu_low_soak_rise_pct_per_min":0.3,)"
                             R"("cpu_low_soak_fall_pct_per_min":0.3,)"
                             R"("cpu_low_soak_max_boost_pct":15.0)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    ExpectThrowsContaining(
        [&]() {
            svg_mb_control::LoadControlLoopConfig(cfg_file.path());
        },
        "cpu_low_soak_max_boost_pct must be <= 10",
        "cpu_low_soak max_boost_pct over cap -> throw");
}

void TestAllFourStagesTogether() {
    // Sanity: full config with every stage configured loads cleanly,
    // and the BoostStage enum order matches the spec table order.
    const std::string body =
        R"(,"thermal_pressure_start_c":84,"thermal_pressure_full_c":95,)"
        R"("thermal_pressure_rise_pct_per_sec":0.04,)"
        R"("thermal_pressure_fall_pct_per_sec":0.02,)"
        R"("thermal_pressure_max_boost_pct":2.0,)"
        R"("midband_pressure_start_c":64,"midband_pressure_full_c":82,)"
        R"("midband_pressure_max_boost_pct":1.5,)"
        R"("gpu_airflow_start_c":58,"gpu_airflow_full_c":72,)"
        R"("gpu_airflow_max_boost_pct":1.0,)"
        R"("cpu_low_soak_start_c":72,"cpu_low_soak_full_c":82,)"
        R"("cpu_low_soak_release_c":68,)"
        R"("cpu_low_soak_rise_pct_per_min":0.3,)"
        R"("cpu_low_soak_fall_pct_per_min":0.3,)"
        R"("cpu_low_soak_max_boost_pct":0.3)";
    TempJsonFile cfg_file(ChannelConfigJson(body));
    const auto loaded = svg_mb_control::LoadControlLoopConfig(cfg_file.path());
    const auto& boosts = loaded.channels[0].boosts;
    ExpectNear(boosts[Idx(svg_mb_control::BoostStage::ThermalPressure)]
                   .start_c,
               84.0, 1e-12,
               "all-four: ThermalPressure index matches spec table");
    ExpectNear(boosts[Idx(svg_mb_control::BoostStage::MidbandPressure)]
                   .start_c,
               64.0, 1e-12,
               "all-four: MidbandPressure index matches spec table");
    ExpectNear(boosts[Idx(svg_mb_control::BoostStage::GpuAirflow)].start_c,
               58.0, 1e-12,
               "all-four: GpuAirflow index matches spec table");
    ExpectNear(boosts[Idx(svg_mb_control::BoostStage::CpuLowSoak)].start_c,
               72.0, 1e-12,
               "all-four: CpuLowSoak index matches spec table");
    ExpectNear(boosts[Idx(svg_mb_control::BoostStage::CpuLowSoak)].release_c,
               68.0, 1e-12,
               "all-four: CpuLowSoak release_c populated");
}

}  // namespace

int main() {
    try {
        TestNoBoostFields_AllStagesInactive();
        TestThermalPressure_CompleteFields();
        TestThermalPressure_PartialOk();
        TestThermalPressure_MissingStartFails();
        TestThermalPressure_FullEqStartFails();
        TestMidbandPressure_RoundTripsThroughLoader();
        TestGpuAirflow_RoundTripsThroughLoader();
        TestCpuLowSoak_CompleteFields();
        TestCpuLowSoak_PartialFails();
        TestCpuLowSoak_ReleaseGtStartFails();
        TestCpuLowSoak_FullEqStartFails();
        TestCpuLowSoak_MaxBoostOver10Fails();
        TestAllFourStagesTogether();
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: unexpected top-level exception: " << ex.what()
                  << '\n';
        ++g_failures;
    }
    if (g_failures > 0) {
        std::cerr << g_failures << " control_loop_config test failure(s)\n";
        return 1;
    }
    std::cout << "control_loop_config tests OK\n";
    return 0;
}
