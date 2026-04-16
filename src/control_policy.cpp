#include "control_policy.h"

#include <algorithm>
#include <stdexcept>

namespace svg_mb_control {

TempBlend ParseTempBlend(const std::string& text) {
    if (text == "cpu_only" || text == "cpu") return TempBlend::CpuOnly;
    if (text == "gpu_only" || text == "gpu") return TempBlend::GpuOnly;
    if (text == "max_cpu_gpu" || text == "max") return TempBlend::MaxCpuGpu;
    throw std::runtime_error("Unknown temp blend: " + text);
}

std::string TempBlendToString(TempBlend blend) {
    switch (blend) {
        case TempBlend::CpuOnly: return "cpu_only";
        case TempBlend::GpuOnly: return "gpu_only";
        case TempBlend::MaxCpuGpu: return "max_cpu_gpu";
    }
    return "cpu_only";
}

double BlendTemps(const TempInputs& inputs, TempBlend mode) {
    constexpr double kAbsoluteZeroC = -273.15;
    const double cpu = inputs.cpu_available ? inputs.cpu_c : kAbsoluteZeroC;
    const double gpu = inputs.gpu_available ? inputs.gpu_c : kAbsoluteZeroC;
    switch (mode) {
        case TempBlend::CpuOnly: return cpu;
        case TempBlend::GpuOnly: return gpu;
        case TempBlend::MaxCpuGpu: return (std::max)(cpu, gpu);
    }
    return cpu;
}

double LookupCurve(const std::vector<CurvePoint>& curve,
                   double temp_c,
                   double min_floor_pct) {
    if (curve.empty()) {
        return (std::max)(0.0, min_floor_pct);
    }
    double raw = 0.0;
    if (temp_c <= curve.front().temp_c) {
        raw = curve.front().duty_pct;
    } else if (temp_c >= curve.back().temp_c) {
        raw = curve.back().duty_pct;
    } else {
        for (std::size_t index = 1u; index < curve.size(); ++index) {
            const CurvePoint& lo = curve[index - 1u];
            const CurvePoint& hi = curve[index];
            if (temp_c <= hi.temp_c) {
                const double span = hi.temp_c - lo.temp_c;
                if (span <= 0.0) {
                    raw = hi.duty_pct;
                } else {
                    const double t = (temp_c - lo.temp_c) / span;
                    raw = lo.duty_pct + t * (hi.duty_pct - lo.duty_pct);
                }
                break;
            }
        }
    }
    const double floor = (std::max)(0.0, min_floor_pct);
    return std::clamp(raw, floor, 100.0);
}

}  // namespace svg_mb_control
