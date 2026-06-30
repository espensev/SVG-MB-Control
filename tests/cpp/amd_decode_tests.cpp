// Unit tests for the pure AMD SMN decode math in src/hardware/amd_decode.h
// (DecodeTctl, DecodeCcdTemp, SelectCcdLayout). The AmdReader's real transport
// is gated behind the PawnIO kernel module and the simulation path injects a
// final Celsius value, so this decode math is otherwise unreachable from the
// test harness. Expected values are computed from the documented formulas:
//   Tctl: (raw >> 21) * 0.125 C, minus 49 C when bit 0x80000 is set
//   CCD:  (raw & 0xFFF) * 0.125 C - 305 C, valid when raw_12bit > 0 and < 125 C

#include "amd_decode.h"

#include "test_helpers.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>

namespace {

constexpr double kEps = 1e-9;

void TestDecodeTctl_Magnitude() {
    using svg_mb_control::amd::DecodeTctl;
    // bits [31:21] in 0.125 C steps. raw>>21 == 0 -> 0 C.
    ExpectNear(DecodeTctl(0u), 0.0, kEps, "DecodeTctl(0) == 0 C");
    // raw>>21 == 400 -> 400 * 0.125 == 50 C. 400 << 21 == 0x32000000.
    ExpectNear(DecodeTctl(0x32000000u), 50.0, kEps,
               "DecodeTctl 400 steps == 50 C");
    // raw>>21 == 520 -> 520 * 0.125 == 65 C. 520 << 21 == 0x41000000.
    ExpectNear(DecodeTctl(0x41000000u), 65.0, kEps,
               "DecodeTctl 520 steps == 65 C");
}

void TestDecodeTctl_OffsetFlag() {
    using svg_mb_control::amd::DecodeTctl;
    // Bit 0x80000 set subtracts a fixed 49 C. 50 C - 49 == 1 C.
    ExpectNear(DecodeTctl(0x32000000u | 0x80000u), 1.0, kEps,
               "DecodeTctl 50 C with offset flag == 1 C");
    // 65 C - 49 == 16 C.
    ExpectNear(DecodeTctl(0x41000000u | 0x80000u), 16.0, kEps,
               "DecodeTctl 65 C with offset flag == 16 C");
}

void TestDecodeTctl_LowBitsIgnored() {
    using svg_mb_control::amd::DecodeTctl;
    // Bits below 21 (other than the 0x80000 flag) do not change the magnitude.
    ExpectNear(DecodeTctl(0x41000000u | 0x7FFu), 65.0, kEps,
               "DecodeTctl ignores sub-step low bits");
}

void TestDecodeTctl_ValidityGate() {
    using svg_mb_control::amd::DecodeTctl;
    // A zero temperature field (e.g. a successful-but-zero SMN read) decodes to
    // 0 C but must be flagged invalid so it is not used as a CPU control input.
    bool valid = true;
    ExpectNear(DecodeTctl(0u, &valid), 0.0, kEps, "DecodeTctl(0) magnitude");
    ExpectFalse(valid, "DecodeTctl raw 0 (zero temp field) is invalid");
    // Offset flag set over a zero temperature field -> -49 C, still invalid
    // because the field is unpopulated.
    valid = true;
    ExpectNear(DecodeTctl(0x80000u, &valid), -49.0, kEps,
               "DecodeTctl offset-only magnitude");
    ExpectFalse(valid, "DecodeTctl zero field with offset flag is invalid");
    // A populated, in-range reading (520 steps -> 65 C) is valid.
    valid = false;
    ExpectNear(DecodeTctl(0x41000000u, &valid), 65.0, kEps,
               "DecodeTctl 65 C magnitude");
    ExpectTrue(valid, "DecodeTctl 65 C is valid");
    // temp must be strictly below 125 C. 1000 steps -> exactly 125 C -> invalid.
    valid = true;
    DecodeTctl(0x7D000000u, &valid);
    ExpectFalse(valid, "DecodeTctl exactly 125 C is invalid");
    // 999 steps -> 124.875 C -> valid.
    valid = false;
    ExpectNear(DecodeTctl(0x7CE00000u, &valid), 124.875, kEps,
               "DecodeTctl 999 steps == 124.875 C");
    ExpectTrue(valid, "DecodeTctl just under 125 C is valid");
}

void TestDecodeTctl_NullValid() {
    using svg_mb_control::amd::DecodeTctl;
    // A null out_valid must not be dereferenced; existing callers pass nothing.
    ExpectNear(DecodeTctl(0x41000000u, nullptr), 65.0, kEps,
               "DecodeTctl null out_valid returns temperature");
}

void TestDecodeCcdTemp_Magnitude() {
    using svg_mb_control::amd::DecodeCcdTemp;
    bool valid = false;
    // raw_12bit == 2440 -> 2440*0.125 - 305 == 0 C.
    ExpectNear(DecodeCcdTemp(2440u, &valid), 0.0, kEps,
               "DecodeCcdTemp 2440 == 0 C");
    ExpectTrue(valid, "DecodeCcdTemp 0 C is valid");
    // raw_12bit == 2840 -> 2840*0.125 - 305 == 50 C.
    ExpectNear(DecodeCcdTemp(2840u, &valid), 50.0, kEps,
               "DecodeCcdTemp 2840 == 50 C");
    ExpectTrue(valid, "DecodeCcdTemp 50 C is valid");
}

void TestDecodeCcdTemp_MasksHighBits() {
    using svg_mb_control::amd::DecodeCcdTemp;
    bool valid = false;
    // Only the low 12 bits matter; high bits are masked off.
    ExpectNear(DecodeCcdTemp(0xFFFFF000u | 2840u, &valid), 50.0, kEps,
               "DecodeCcdTemp masks to low 12 bits");
    ExpectTrue(valid, "DecodeCcdTemp masked value still valid");
}

void TestDecodeCcdTemp_ValidityGate() {
    using svg_mb_control::amd::DecodeCcdTemp;
    // raw_12bit == 0 -> invalid (unpopulated sensor).
    bool valid = true;
    DecodeCcdTemp(0u, &valid);
    ExpectFalse(valid, "DecodeCcdTemp raw_12bit 0 is invalid");
    // temp must be strictly below 125 C. 3440 -> exactly 125 C -> invalid.
    valid = true;
    DecodeCcdTemp(3440u, &valid);
    ExpectFalse(valid, "DecodeCcdTemp exactly 125 C is invalid");
    // 3439 -> 124.875 C -> valid.
    valid = false;
    const double just_under = DecodeCcdTemp(3439u, &valid);
    ExpectNear(just_under, 124.875, kEps, "DecodeCcdTemp 3439 == 124.875 C");
    ExpectTrue(valid, "DecodeCcdTemp just under 125 C is valid");
    // Max raw_12bit (0xFFF) -> 206.875 C -> out-of-range invalid.
    valid = true;
    DecodeCcdTemp(0xFFFu, &valid);
    ExpectFalse(valid, "DecodeCcdTemp 0xFFF is out-of-range invalid");
}

void TestDecodeCcdTemp_NullValid() {
    using svg_mb_control::amd::DecodeCcdTemp;
    // A null out_valid must not be dereferenced.
    ExpectNear(DecodeCcdTemp(2840u, nullptr), 50.0, kEps,
               "DecodeCcdTemp null out_valid returns temperature");
}

void TestSelectCcdLayout_Zen2() {
    using svg_mb_control::amd::SelectCcdLayout;
    using svg_mb_control::amd::kCcdTempZen2Base;
    for (std::uint32_t model : {0x31u, 0x71u, 0x21u}) {
        std::uint32_t base = 0u;
        ExpectTrue(SelectCcdLayout(model, &base),
                   "SelectCcdLayout Zen2 model supported");
        ExpectTrue(base == kCcdTempZen2Base, "SelectCcdLayout Zen2 base");
    }
}

void TestSelectCcdLayout_Zen4() {
    using svg_mb_control::amd::SelectCcdLayout;
    using svg_mb_control::amd::kCcdTempZen4Base;
    for (std::uint32_t model : {0x61u, 0x44u}) {
        std::uint32_t base = 0u;
        ExpectTrue(SelectCcdLayout(model, &base),
                   "SelectCcdLayout Zen4 model supported");
        ExpectTrue(base == kCcdTempZen4Base, "SelectCcdLayout Zen4 base");
    }
}

void TestSelectCcdLayout_Unsupported() {
    using svg_mb_control::amd::SelectCcdLayout;
    for (std::uint32_t model : {0x00u, 0x40u, 0x99u}) {
        std::uint32_t base = 0xDEADBEEFu;
        ExpectFalse(SelectCcdLayout(model, &base),
                    "SelectCcdLayout unsupported model returns false");
        ExpectTrue(base == 0u, "SelectCcdLayout unsupported model zeroes base");
    }
    // A null out_base must not be dereferenced.
    ExpectTrue(SelectCcdLayout(0x31u, nullptr),
               "SelectCcdLayout null out_base still reports support");
    ExpectFalse(SelectCcdLayout(0x00u, nullptr),
                "SelectCcdLayout null out_base unsupported still false");
}

}  // namespace

int main() {
    TestDecodeTctl_Magnitude();
    TestDecodeTctl_OffsetFlag();
    TestDecodeTctl_LowBitsIgnored();
    TestDecodeTctl_ValidityGate();
    TestDecodeTctl_NullValid();
    TestDecodeCcdTemp_Magnitude();
    TestDecodeCcdTemp_MasksHighBits();
    TestDecodeCcdTemp_ValidityGate();
    TestDecodeCcdTemp_NullValid();
    TestSelectCcdLayout_Zen2();
    TestSelectCcdLayout_Zen4();
    TestSelectCcdLayout_Unsupported();
    if (g_failures > 0) {
        std::cerr << g_failures << " amd_decode test failure(s)\n";
        return 1;
    }
    std::cout << "amd_decode tests OK\n";
    return 0;
}
