#include "control_policy.h"
#include "control_scheduler.h"
#include "csv_util.h"
#include "runtime_snapshot.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void ExpectTrue(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectNear(double actual,
                double expected,
                double tolerance,
                const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected " << expected
                  << " got " << actual << '\n';
    }
}

void ExpectEqual(const std::string& actual,
                 const std::string& expected,
                 const char* message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected [" << expected
                  << "] got [" << actual << "]\n";
    }
}

void TestLookupCurve() {
    const std::vector<svg_mb_control::CurvePoint> curve{
        {30.0, 20.0},
        {50.0, 60.0},
        {70.0, 100.0},
    };

    ExpectNear(svg_mb_control::LookupCurve(curve, 20.0, 0.0),
               20.0,
               0.001,
               "curve clamps below first point");
    ExpectNear(svg_mb_control::LookupCurve(curve, 40.0, 0.0),
               40.0,
               0.001,
               "curve linearly interpolates between points");
    ExpectNear(svg_mb_control::LookupCurve(curve, 80.0, 0.0),
               100.0,
               0.001,
               "curve clamps above last point");
    ExpectNear(svg_mb_control::LookupCurve(curve, 30.0, 35.0),
               35.0,
               0.001,
               "curve respects minimum floor");
}

void TestBlendTemps() {
    svg_mb_control::TempInputs inputs;
    inputs.cpu_c = 61.0;
    inputs.cpu_available = true;
    inputs.gpu_c = 55.0;
    inputs.gpu_available = true;

    ExpectNear(svg_mb_control::BlendTemps(inputs,
                                          svg_mb_control::TempBlend::CpuOnly),
               61.0,
               0.001,
               "CPU-only blend selects CPU temperature");
    ExpectNear(svg_mb_control::BlendTemps(inputs,
                                          svg_mb_control::TempBlend::GpuOnly),
               55.0,
               0.001,
               "GPU-only blend selects GPU temperature");
    ExpectNear(svg_mb_control::BlendTemps(inputs,
                                          svg_mb_control::TempBlend::MaxCpuGpu),
               61.0,
               0.001,
               "max blend selects hotter source");
    ExpectNear(
        svg_mb_control::BlendTemps(
            inputs,
            svg_mb_control::TempBlend::MaxCpuGpuSourceAware),
        55.0,
        0.001,
        "source-aware max blend selects GPU primary source");

    inputs.gpu_available = false;
    ExpectNear(
        svg_mb_control::BlendTemps(
            inputs,
            svg_mb_control::TempBlend::MaxCpuGpuSourceAware),
        61.0,
        0.001,
        "source-aware max blend falls back to CPU when GPU is missing");
    ExpectNear(svg_mb_control::BlendTemps(inputs,
                                          svg_mb_control::TempBlend::GpuOnly),
               -273.15,
               0.001,
               "missing selected temperature reports unavailable sentinel");
}

void TestCsvEscape() {
    ExpectEqual(svg_mb_control::CsvEscape("plain"), "plain",
                "plain CSV field is unchanged");
    ExpectEqual(svg_mb_control::CsvEscape("a,b"), "\"a,b\"",
                "comma CSV field is quoted");
    ExpectEqual(svg_mb_control::CsvEscape("a\"b"), "\"a\"\"b\"",
                "quote CSV field doubles embedded quote");

    std::ostringstream csv;
    svg_mb_control::AppendCsvString(csv, "a,b");
    csv << ',';
    svg_mb_control::AppendCsvDouble(csv, 12.34567, 2);
    csv << ',';
    svg_mb_control::AppendCsvBool(csv, true);
    csv << ',';
    svg_mb_control::AppendCsvInt32WhenAvailable(csv, -1);
    ExpectEqual(csv.str(), "\"a,b\",12.35,true,",
                "CSV append helpers use stable field formatting");
}

void TestRuntimeSnapshotIndex() {
    using namespace svg_mb_control;

    RuntimeSnapshot snapshot;
    RuntimeFanSnapshot first_fan;
    first_fan.channel = 2u;
    first_fan.duty_percent = 31.0;
    snapshot.fans.push_back(first_fan);
    RuntimeFanSnapshot duplicate_fan;
    duplicate_fan.channel = 2u;
    duplicate_fan.duty_percent = 44.0;
    snapshot.fans.push_back(duplicate_fan);

    RuntimeAmdSensor first_sensor;
    first_sensor.label = "Tctl/Tdie";
    first_sensor.temperature_c = 66.5;
    snapshot.amd_sensors.push_back(first_sensor);
    RuntimeAmdSensor duplicate_sensor;
    duplicate_sensor.label = "Tctl/Tdie";
    duplicate_sensor.temperature_c = 77.5;
    snapshot.amd_sensors.push_back(duplicate_sensor);

    RuntimeSnapshotIndex index;
    index.Rebuild(snapshot);

    const RuntimeFanSnapshot* fan = index.FindFanChannel(2u);
    ExpectTrue(fan == &snapshot.fans.front(),
               "snapshot index preserves first fan match");
    ExpectNear(index.FindAmdSensorTemperature("Tctl/Tdie"), 66.5, 0.001,
               "snapshot index preserves first AMD sensor match");
    ExpectTrue(index.FindFanChannel(9u) == nullptr,
               "snapshot index missing fan returns null");
    ExpectTrue(std::isnan(index.FindAmdSensorTemperature("missing")),
               "snapshot index missing AMD sensor returns NaN");

    RuntimeSnapshot empty;
    index.Rebuild(empty);
    ExpectTrue(index.FindFanChannel(2u) == nullptr,
               "snapshot index rebuild drops old fan entries");
    ExpectTrue(std::isnan(index.FindAmdSensorTemperature("Tctl/Tdie")),
               "snapshot index rebuild drops old AMD sensor entries");
}

// FEAT-0002: whole-system CPU busy derivation from GetSystemTimes deltas.
// Guards the no-divide-by-processor-count rule and the no-false-zero behavior
// (docs/cpu-settings-evidence-logger-decision-2026-06-04.md).
void TestSystemCpuBusyDerivation() {
    using svg_mb_control::ProcessResourceSample;
    using svg_mb_control::RuntimeControlLoopTimingState;
    using svg_mb_control::UpdateTimingResources;

    // Deltas (100 ns units): idle 4,000,000 (=400 ms), kernel 10,000,000
    // (=1000 ms, includes idle), user 2,000,000 (=200 ms). total=kernel+user
    // = 12,000,000; busy = total-idle = 8,000,000 -> 66.667% busy.
    ProcessResourceSample previous;
    previous.valid_system_cpu = true;
    previous.system_idle_100ns = 10'000'000u;
    previous.system_kernel_100ns = 50'000'000u;
    previous.system_user_100ns = 20'000'000u;

    ProcessResourceSample current;
    current.valid_system_cpu = true;
    current.system_idle_100ns = 14'000'000u;
    current.system_kernel_100ns = 60'000'000u;
    current.system_user_100ns = 22'000'000u;

    RuntimeControlLoopTimingState timing;
    UpdateTimingResources(&timing, previous, current, /*have_previous=*/true,
                          /*processor_count=*/8u);

    // Must be the ratio of whole-machine aggregates, NOT divided by the 8
    // processors (a wrong /count would yield ~8.33).
    ExpectNear(timing.system_cpu_busy_pct, 66.667, 0.01,
               "system_cpu_busy_pct is core-normalized, not divided by count");
    ExpectNear(timing.system_cpu_idle_delta_ms, 400.0, 0.001,
               "system_cpu_idle_delta_ms");
    ExpectNear(timing.system_cpu_kernel_delta_ms, 1000.0, 0.001,
               "system_cpu_kernel_delta_ms");
    ExpectNear(timing.system_cpu_user_delta_ms, 200.0, 0.001,
               "system_cpu_user_delta_ms");
    ExpectTrue(timing.system_cpu_processor_count == 8u,
               "system_cpu_processor_count recorded");

    // No previous sample -> no false zero; fields stay blank/NaN, count 0.
    RuntimeControlLoopTimingState first_tick;
    UpdateTimingResources(&first_tick, previous, current,
                          /*have_previous=*/false, /*processor_count=*/8u);
    ExpectTrue(std::isnan(first_tick.system_cpu_busy_pct),
               "system_cpu_busy_pct blank with no previous sample");
    ExpectTrue(first_tick.system_cpu_processor_count == 0u,
               "system_cpu_processor_count blank with no previous sample");

    // Counter moved backwards (wrap / reset) -> guarded, no value emitted.
    ProcessResourceSample regressed = current;
    regressed.system_idle_100ns = previous.system_idle_100ns - 1u;
    RuntimeControlLoopTimingState wrapped;
    UpdateTimingResources(&wrapped, previous, regressed,
                          /*have_previous=*/true, /*processor_count=*/8u);
    ExpectTrue(std::isnan(wrapped.system_cpu_busy_pct),
               "system_cpu_busy_pct blank when a counter regresses");
}

}  // namespace

int main() {
    TestLookupCurve();
    TestBlendTemps();
    TestCsvEscape();
    TestRuntimeSnapshotIndex();
    TestSystemCpuBusyDerivation();

    if (g_failures != 0) {
        std::cerr << g_failures << " core smoke test failure(s)\n";
        return 1;
    }

    return 0;
}
