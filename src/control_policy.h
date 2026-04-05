#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace svg_mb_control {

// A single point on a fan curve: at temp_c, command duty_pct.
struct CurvePoint {
    double temp_c = 0.0;
    double duty_pct = 0.0;
};

// How a channel blends CPU and GPU temperature into a single control
// input. Per-channel, so radiator fans can use cpu_only while case fans
// use max_cpu_gpu.
enum class TempBlend {
    CpuOnly,
    GpuOnly,
    MaxCpuGpu,
};

TempBlend ParseTempBlend(const std::string& text);
std::string TempBlendToString(TempBlend blend);

// Per-cycle temperature readings fed to the policy. Missing sources are
// flagged via the *_available bits.
struct TempInputs {
    double cpu_c = 0.0;
    bool cpu_available = false;
    std::string cpu_label;
    double gpu_c = 0.0;
    bool gpu_available = false;
    std::string gpu_label;
};

// Combines inputs per the blend mode. Returns the selected temperature,
// or a large negative number if the selected source is unavailable.
double BlendTemps(const TempInputs& inputs, TempBlend mode);

// Piecewise-linear interpolation over the curve. `curve` must be sorted
// by `temp_c`. Values below the first point clamp to the first point's
// duty; values above the last point clamp to the last point's duty. The
// result is then clamped to [min_floor_pct, 100.0].
double LookupCurve(const std::vector<CurvePoint>& curve,
                   double temp_c,
                   double min_floor_pct);

// Reads a labeled AMD sensor temperature from a current_state snapshot
// JSON. Returns NaN when the label is not found. Matches on exact label
// equality.
double ExtractAmdSensorTemperature(const std::string& snapshot_json,
                                   const std::string& label);

struct FanRawState {
    bool present = false;
    std::uint8_t duty_raw = 0u;
    std::uint8_t mode_raw = 0u;
};

// Reads duty_raw and mode_raw for the given channel from the fans array
// in a current_state snapshot JSON. Returns `present=false` when the
// channel is not present.
FanRawState ExtractFanRawState(const std::string& snapshot_json,
                               std::uint32_t channel);

}  // namespace svg_mb_control
