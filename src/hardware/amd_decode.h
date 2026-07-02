#ifndef SVG_MB_CONTROL_HARDWARE_AMD_DECODE_H
#define SVG_MB_CONTROL_HARDWARE_AMD_DECODE_H

#include <cstdint>

// Pure AMD SMN raw-register -> Celsius decode math, split out of
// amd_reader.cpp so it is reachable from a unit test. The reader's real
// transport is gated behind the PawnIO kernel module and the simulation path
// injects a final Celsius value, so this math is otherwise unreachable from the
// hermetic harness. These functions hold the numeric contract for Tctl/Tdie and
// per-CCD temperatures and the Zen model -> CCD register-base mapping; keep them
// and tests/cpp/amd_decode_tests.cpp in sync with any decode change.
namespace svg_mb_control {
namespace amd {

// Bit 19 of the Tctl/Tdie register: when set, the reading carries the fixed
// -49 C temperature offset used by the affected parts.
inline constexpr std::uint32_t kTctlTempOffsetFlag = 0x80000u;

// Per-CCD temperature register base addresses by microarchitecture.
inline constexpr std::uint32_t kCcdTempZen2Base = 0x00059954u;
inline constexpr std::uint32_t kCcdTempZen4Base = 0x00059B08u;

// Decodes the Tctl/Tdie SMN register: bits [31:21] are the temperature in
// 0.125 C steps, with a fixed -49 C offset applied when kTctlTempOffsetFlag is
// set. *out_valid (when non-null) reports whether the reading is a populated,
// in-range sensor (non-zero temperature field and below 125 C); callers skip
// samples that fail this gate so a successful-but-zero SMN read does not
// surface as a valid ~0 C CPU control input. Mirrors the DecodeCcdTemp gate.
inline double DecodeTctl(std::uint32_t raw, bool* out_valid = nullptr) {
    const std::uint32_t temp_field = raw >> 21;
    double temp = static_cast<double>((temp_field * 125u) * 0.001);
    if ((raw & kTctlTempOffsetFlag) != 0u) {
        temp -= 49.0;
    }
    if (out_valid != nullptr) {
        *out_valid = (temp_field > 0u && temp < 125.0);
    }
    return temp;
}

// Decodes a per-CCD temperature register: the low 12 bits are the temperature
// in 0.125 C steps biased by -305 C. *out_valid (when non-null) reports whether
// the reading is a populated, in-range sensor (non-zero raw and below 125 C);
// callers skip samples that fail this gate.
inline double DecodeCcdTemp(std::uint32_t raw, bool* out_valid) {
    const std::uint32_t raw_12bit = raw & 0xFFFu;
    const double temp =
        ((static_cast<double>(raw_12bit * 125u) - 305000.0) * 0.001);
    if (out_valid != nullptr) {
        *out_valid = (raw_12bit > 0u && temp < 125.0);
    }
    return temp;
}

// Maps a CPUID display-model to its per-CCD register base. Returns false for
// models without a known CCD layout, setting *out_base (when non-null) to 0.
inline bool SelectCcdLayout(std::uint32_t cpu_model, std::uint32_t* out_base) {
    switch (cpu_model) {
        case 0x31u:
        case 0x71u:
        case 0x21u:
            if (out_base != nullptr) {
                *out_base = kCcdTempZen2Base;
            }
            return true;
        case 0x61u:
        case 0x44u:
            if (out_base != nullptr) {
                *out_base = kCcdTempZen4Base;
            }
            return true;
        default:
            if (out_base != nullptr) {
                *out_base = 0u;
            }
            return false;
    }
}

}  // namespace amd
}  // namespace svg_mb_control

#endif  // SVG_MB_CONTROL_HARDWARE_AMD_DECODE_H
