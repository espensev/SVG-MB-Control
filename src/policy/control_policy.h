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

// How a channel selects the primary control temperature. Per-channel, so
// radiator fans can use cpu_only while case fans use a GPU/CPU max variant.
enum class TempBlend {
    CpuOnly,
    GpuOnly,
    MaxCpuGpu,
    MaxCpuGpuSourceAware,
};

enum class CurveShape {
    Linear,
    SmootherStep,
};

// Per-channel control-law selector (FEAT-0003 REQ-PROFILE-03). Absent in config
// means CurveOverlay, so existing configs keep today's behavior unedited.
enum class ControllerKind {
    CurveOverlay,
    Pid,
};

// PID feed-forward bias source (FEAT-0003 decision D3a). Curve adds the PID
// terms on top of the existing temperature->duty curve; Fixed adds them on top
// of a constant resting duty.
enum class PidFeedforward {
    Curve,
    Fixed,
};

TempBlend ParseTempBlend(const std::string& text);
std::string TempBlendToString(TempBlend blend);
CurveShape ParseCurveShape(const std::string& text);
std::string CurveShapeToString(CurveShape shape);
ControllerKind ParseControllerKind(const std::string& text);
std::string ControllerKindToString(ControllerKind kind);
PidFeedforward ParsePidFeedforward(const std::string& text);
std::string PidFeedforwardToString(PidFeedforward feedforward);

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
double LookupCurve(const std::vector<CurvePoint>& curve,
                   double temp_c,
                   double min_floor_pct,
                   CurveShape shape);

}  // namespace svg_mb_control
