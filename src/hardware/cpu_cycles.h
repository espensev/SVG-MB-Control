#pragma once

// Pure, dependency-free math for AMD APERF/MPERF cycle evidence (FEAT-0006,
// REQ-CPUEFF-01 work numerator + REQ-CPUEFF-03 effective-frequency context).
// Header-only and free of Windows/transport deps so it is unit-testable without
// hardware (mirrors src/hardware/rapl_energy.h and amd_decode.h). The live
// per-core MSR read and provenance live in amd_reader.cpp; the analyzer reuses
// AperfMperfRatio / EffectiveFrequencyMhz for the derived figures.
//
// Source MSRs (read-only AMD aliases; AMD OSRR 56255): MPERF_RO 0xC00000E7
// counts at the P0 (base) frequency while the core is in C0; APERF_RO 0xC00000E8
// counts in proportion to the actual core clock while in C0. Effective frequency
// = (dAPERF / dMPERF) * P0; delivered cycles (the work numerator) = dAPERF. The
// architectural IA32 0xE7/0xE8 are NOT what AMDFamily17.p allow-lists -- reading
// those returns ACCESS_DENIED (the 2026-06-07 probe-index error).

#include <cstdint>
#include <limits>
#include <string_view>

namespace svg_mb_control {
namespace cycles {

// The only two MSRs the cycle path is permitted to read (REQ-CPUEFF-05/06).
inline constexpr std::uint32_t kMsrMperfRo = 0xC00000E7u;  // P0-rate clock count
inline constexpr std::uint32_t kMsrAperfRo = 0xC00000E8u;  // actual-rate count

// True only for an allow-listed read index. Any other index (and every write)
// is rejected by the helper that calls this.
inline bool IsAllowlistedCycleMsr(std::uint32_t index) {
    return index == kMsrMperfRo || index == kMsrAperfRo;
}

// Producer-side acquisition gate (REQ-CPUEFF-05: the read is opt-in). The MSRs
// are read only when SVG_MB_CONTROL_CPU_CYCLES_MODE is exactly "enabled";
// anything else -- unset (caller defaults to "disabled"), empty, wrong case --
// stays off, so the default is no read and null columns. Mirrors
// rapl::EnergyEnabledFromMode; separate gate so cycles enable independently of
// energy.
inline bool CyclesEnabledFromMode(std::string_view mode) {
    return mode == "enabled";
}

// 64-bit modular delta of a free-running cycle counter. APERF/MPERF are 64-bit
// and do not wrap within any realistic window, but unsigned subtraction is
// wraparound-safe regardless.
inline std::uint64_t CounterDelta(std::uint64_t prev, std::uint64_t cur) {
    return cur - prev;
}

// APERF/MPERF ratio = actual / P0 (the effective multiplier over base). NaN when
// dMPERF is zero -- the core did no C0 work over the window, so the ratio is
// undefined (no false value).
inline double AperfMperfRatio(std::uint64_t d_aperf, std::uint64_t d_mperf) {
    if (d_mperf == 0u) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(d_aperf) / static_cast<double>(d_mperf);
}

// Effective frequency (MHz) = ratio * P0 (base) MHz. NaN when the ratio is NaN
// or the base is not positive. Derived figure (analyzer); the logger records the
// raw deltas, not this (REQ-CPUEFF-07).
inline double EffectiveFrequencyMhz(std::uint64_t d_aperf, std::uint64_t d_mperf,
                                    double p0_mhz) {
    if (!(p0_mhz > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return AperfMperfRatio(d_aperf, d_mperf) * p0_mhz;
}

// Implausibility guard (mirror of rapl::WindowImplausible). Returns true when the
// window must be blanked (emit no value -- no false zero):
//   - window missing/non-positive or longer than dt_cap_ms (stale / gap), or
//   - dMPERF == 0 (no C0 activity -> ratio undefined), or
//   - ratio outside (0, ratio_cap] -- garbage, or paired reads that straddled a
//     core migration (the affinity-untrusted failure mode).
inline bool CycleWindowImplausible(std::uint64_t d_aperf, std::uint64_t d_mperf,
                                   double window_ms, double dt_cap_ms,
                                   double ratio_cap) {
    if (!(window_ms > 0.0) || window_ms > dt_cap_ms || d_mperf == 0u) {
        return true;
    }
    const double ratio = AperfMperfRatio(d_aperf, d_mperf);
    return !(ratio > 0.0) || ratio > ratio_cap;
}

struct CycleWindowResult {
    std::uint64_t sample_id = 0u;  // 0 until the first real delta
    double window_ms = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t d_aperf = 0u;
    std::uint64_t d_mperf = 0u;
    bool blanked = false;   // guard fired: deltas are not trustworthy, emit none
    bool baseline = true;   // this read only established the baseline
};

// Pure per-window transition for a cycle read that is DUE and SUCCEEDED (mirror
// of rapl::AdvanceEnergyWindow). The caller owns cadence, the per-core pinned
// read, the disabled/hash gate, and the clock; this owns baseline-vs-delta, the
// sample-id increment, and the implausibility blank, so it is unit-testable
// without hardware. Mutates *have_prev / *prev_aperf / *prev_mperf / *sample_id.
//   - first call (have_prev=false): establishes the baseline, no delta, id
//     unchanged.
//   - later calls: id increments; deltas are the modular counter deltas, or
//     blanked (blanked=true, deltas left 0) when the guard fires -- the id still
//     advances because the window happened. The caller must emit empty columns
//     when baseline or blanked (never a false zero).
inline CycleWindowResult AdvanceCycleWindow(bool* have_prev,
                                            std::uint64_t* prev_aperf,
                                            std::uint64_t* prev_mperf,
                                            std::uint64_t* sample_id,
                                            std::uint64_t cur_aperf,
                                            std::uint64_t cur_mperf,
                                            double window_ms, double dt_cap_ms,
                                            double ratio_cap) {
    CycleWindowResult r;
    if (!*have_prev) {
        *prev_aperf = cur_aperf;
        *prev_mperf = cur_mperf;
        *have_prev = true;
        r.sample_id = *sample_id;  // unchanged (0 until first delta)
        r.baseline = true;
        return r;
    }
    const std::uint64_t d_aperf = CounterDelta(*prev_aperf, cur_aperf);
    const std::uint64_t d_mperf = CounterDelta(*prev_mperf, cur_mperf);
    *prev_aperf = cur_aperf;
    *prev_mperf = cur_mperf;
    ++*sample_id;
    r.sample_id = *sample_id;
    r.window_ms = window_ms;
    r.baseline = false;
    if (CycleWindowImplausible(d_aperf, d_mperf, window_ms, dt_cap_ms,
                               ratio_cap)) {
        r.blanked = true;  // deltas left 0; caller emits empty (no false zero)
        return r;
    }
    r.d_aperf = d_aperf;
    r.d_mperf = d_mperf;
    return r;
}

}  // namespace cycles
}  // namespace svg_mb_control
