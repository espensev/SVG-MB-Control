#include "numeric_parse.h"

#include "test_helpers.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

void TestDoubleParsing() {
    double value = 0.0;
    ExpectTrue(svg_mb_control::TryParseDoubleStrict(" 12.5 ", value),
               "double parser accepts trimmed finite value");
    ExpectNear(value, 12.5, 1e-9, "double parser value");
    ExpectTrue(svg_mb_control::TryParseDoubleStrict("-0.25", value),
               "double parser accepts negative value");
    ExpectNear(value, -0.25, 1e-9, "double parser negative value");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("", value),
                "double parser rejects empty string");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("12.5x", value),
                "double parser rejects trailing junk");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("12 5", value),
                "double parser rejects embedded junk");
}

void TestNonFiniteRejection() {
    double value = 0.0;
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("inf", value),
                "double parser rejects non-finite inf");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("infinity", value),
                "double parser rejects non-finite infinity");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("nan", value),
                "double parser rejects non-finite nan");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("-inf", value),
                "double parser rejects non-finite -inf");
}

// Pins the intentional from_chars contract that is stricter than the
// std::stod/std::stoul these helpers replaced: a leading '+', a hex/0x prefix,
// a sign-only token, and a whitespace-only token are all rejected (the caller
// falls back). Locking these so the narrowing stays deliberate, not incidental.
void TestStrictAcceptanceSet() {
    double dval = 0.0;
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("+12.5", dval),
                "double parser rejects leading plus");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("0x10", dval),
                "double parser rejects hex prefix");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("+", dval),
                "double parser rejects sign-only token");
    ExpectFalse(svg_mb_control::TryParseDoubleStrict("   ", dval),
                "double parser rejects whitespace-only token");

    std::uint32_t uval = 0u;
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("+42", uval),
                "uint32 parser rejects leading plus");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("0x10", uval),
                "uint32 parser rejects hex prefix");

    std::int32_t ival = 0;
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("+7", ival),
                "int32 parser rejects leading plus");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("-", ival),
                "int32 parser rejects sign-only token");
}

void TestUnsignedParsing() {
    std::uint32_t value = 0u;
    ExpectTrue(svg_mb_control::TryParseIntegralStrict("42", value),
               "uint32 parser accepts decimal value");
    ExpectTrue(value == 42u, "uint32 parser value");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("-1", value),
                "uint32 parser rejects negative sign");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("42x", value),
                "uint32 parser rejects trailing junk");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict(
                    "4294967296", value),
                "uint32 parser rejects overflow");
}

void TestSignedParsing() {
    std::int32_t value = 0;
    ExpectTrue(svg_mb_control::TryParseIntegralStrict("-7", value),
               "int32 parser accepts negative value");
    ExpectTrue(value == -7, "int32 parser value");
    ExpectFalse(svg_mb_control::TryParseIntegralStrict("7ms", value),
                "int32 parser rejects trailing unit");
}

}  // namespace

int main() {
    TestDoubleParsing();
    TestNonFiniteRejection();
    TestStrictAcceptanceSet();
    TestUnsignedParsing();
    TestSignedParsing();
    if (g_failures != 0) {
        std::cerr << g_failures << " numeric_parse test failure(s)\n";
        return 1;
    }
    std::cout << "numeric_parse tests OK\n";
    return 0;
}
