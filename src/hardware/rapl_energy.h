#pragma once

// Pure, dependency-free math for AMD RAPL package-energy evidence (FEAT-0006,
// REQ-CPUEFF-02). Kept header-only and free of Windows/transport deps so it is
// unit-testable without hardware (mirrors src/hardware/amd_decode.h). The live
// MSR read and provenance live in amd_reader.cpp; the analyzer reuses
// AveragePackageWatts for the derived watt figure.
//
// Source MSRs (read-only): MSR_RAPL_PWR_UNIT 0xC0010299 (energy unit),
// MSR_PKG_ENERGY_STAT 0xC001029B (socket-level 32-bit energy accumulator).
// See docs/cpu-work-energy-acquisition-decision-2026-06-07.md §Derivation.

#include <cstdint>
#include <limits>

namespace svg_mb_control {
namespace rapl {

// The only two MSRs the energy path is permitted to read (REQ-CPUEFF-05/06).
// Single source of truth for the read-only allow-list guard.
inline constexpr std::uint32_t kMsrRaplPwrUnit = 0xC0010299u;    // energy unit
inline constexpr std::uint32_t kMsrPkgEnergyStat = 0xC001029Bu;  // pkg energy

// True only for an allow-listed read index. Any other index (and every write)
// is rejected by the helper that calls this.
inline bool IsAllowlistedEnergyMsr(std::uint32_t index) {
    return index == kMsrRaplPwrUnit || index == kMsrPkgEnergyStat;
}

// Energy Status Unit is bits [12:8] of MSR_RAPL_PWR_UNIT. The energy unit is
// 1/2^ESU joules, i.e. 1e6/2^ESU microjoules per accumulator unit. The default
// ESU = 16 -> 15.2588 uj/unit. Decode only; plausibility (ESU != 0, in range)
// is the caller's check.
inline double EnergyUnitMicrojoules(std::uint64_t pwr_unit_raw) {
    const std::uint32_t esu =
        (static_cast<std::uint32_t>(pwr_unit_raw) >> 8) & 0x1Fu;
    const double denom = static_cast<double>(std::uint64_t{1} << esu);
    return 1.0e6 / denom;
}

// 32-bit modular delta of the package-energy accumulator. The counter is 32-bit
// and wraps (~65.5 kJ at the default unit), so the delta MUST be computed mod
// 2^32. Exact within a single sub-wrap window; a multi-wrap gap is caught by
// WindowImplausible's dt branch, not here.
inline std::uint32_t PackageEnergyUnitsDelta(std::uint64_t prev_raw,
                                             std::uint64_t cur_raw) {
    return static_cast<std::uint32_t>((cur_raw & 0xFFFFFFFFu) -
                                      (prev_raw & 0xFFFFFFFFu));
}

// Microjoules dissipated over a window, from the modular units delta.
inline double PackageEnergyDeltaMicrojoules(std::uint64_t prev_raw,
                                            std::uint64_t cur_raw,
                                            double uj_per_unit) {
    return static_cast<double>(PackageEnergyUnitsDelta(prev_raw, cur_raw)) *
           uj_per_unit;
}

// Average package power (watts) from a microjoule delta over a window (ms).
// NaN when the window is not positive. Derived figure (analyzer + tests);
// the logger records the raw delta + window, not this.
inline double AveragePackageWatts(double delta_uj, double window_ms) {
    if (!(window_ms > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (delta_uj / 1.0e6) / (window_ms / 1.0e3);
}

// Implausibility guard (decision §Derivation). Returns true when the window
// must be blanked (emit no value -- no false zero). Two independent branches:
//   - dt bound: window missing/non-positive or longer than dt_cap_ms. This is
//     load-bearing: over a long gap the 32-bit counter can multi-wrap and alias
//     to a plausible *low* power the ceiling branch cannot catch.
//   - high-power ceiling: implied average power above ceiling_w (a short-window
//     over-read, or a backward counter step that the modular subtraction turns
//     into a near-2^32 delta).
inline bool WindowImplausible(double delta_uj, double window_ms,
                              double ceiling_w, double dt_cap_ms) {
    if (!(window_ms > 0.0) || window_ms > dt_cap_ms) {
        return true;
    }
    const double watts = AveragePackageWatts(delta_uj, window_ms);
    return !(watts >= 0.0) || watts > ceiling_w;
}

struct EnergyWindowResult {
    std::uint64_t sample_id = 0u;  // 0 until the first real delta
    double window_ms = std::numeric_limits<double>::quiet_NaN();
    double delta_uj = std::numeric_limits<double>::quiet_NaN();
    bool baseline = true;  // true when this read only established the baseline
};

// Pure per-window transition for a resource-window read that is DUE and
// SUCCEEDED. The caller owns cadence gating, the MSR read, the disabled / hash
// gate, the clock, and the not-due mirror; this owns the risky bit -- baseline
// vs delta, the sample-id increment, and the implausibility blank -- so it can
// be unit-tested without hardware. Mutates *have_prev / *prev_raw / *sample_id.
//   - first call (have_prev=false): establishes the baseline, no delta, id
//     unchanged.
//   - later calls: id increments; delta = modular energy delta, or blank (NaN)
//     when the guard fires (the id still advances -- the window happened).
inline EnergyWindowResult AdvanceEnergyWindow(bool* have_prev,
                                              std::uint64_t* prev_raw,
                                              std::uint64_t* sample_id,
                                              std::uint64_t cur_raw,
                                              double window_ms,
                                              double uj_per_unit,
                                              double ceiling_w,
                                              double dt_cap_ms) {
    EnergyWindowResult r;
    if (!*have_prev) {
        *prev_raw = cur_raw;
        *have_prev = true;
        r.sample_id = *sample_id;  // unchanged (0 until first delta)
        r.baseline = true;
        return r;
    }
    const double delta_uj =
        PackageEnergyDeltaMicrojoules(*prev_raw, cur_raw, uj_per_unit);
    *prev_raw = cur_raw;
    ++*sample_id;
    r.sample_id = *sample_id;
    r.window_ms = window_ms;
    r.delta_uj = WindowImplausible(delta_uj, window_ms, ceiling_w, dt_cap_ms)
                     ? std::numeric_limits<double>::quiet_NaN()
                     : delta_uj;
    r.baseline = false;
    return r;
}

}  // namespace rapl
}  // namespace svg_mb_control
