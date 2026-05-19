#include "control_policy.h"
#include "csv_util.h"

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

    inputs.gpu_available = false;
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

}  // namespace

int main() {
    TestLookupCurve();
    TestBlendTemps();
    TestCsvEscape();

    if (g_failures != 0) {
        std::cerr << g_failures << " core smoke test failure(s)\n";
        return 1;
    }

    return 0;
}
