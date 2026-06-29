#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace svg_mb_control {

// Strict, allocation-free numeric parsing for sim/env values, built on
// std::from_chars. Contract (intentional, pinned by numeric_parse_tests.cpp):
//   * Leading and trailing ASCII whitespace is trimmed before parsing.
//   * Base-10 only: a "0x"/"0"-octal prefix is rejected, not reinterpreted.
//   * A leading '+' is rejected (from_chars accepts only '-'); this is stricter
//     than the std::stod/std::stoul these helpers replaced, and is deliberate.
//   * Any trailing non-whitespace data rejects the whole token (no partial
//     parse) -- the defect this header was introduced to fix.
//   * Doubles must be finite: "inf"/"nan" are rejected so a stray env/sensor
//     value falls back to the caller's default instead of poisoning the value.
//   * Integral overflow/underflow returns false, indistinguishable from
//     malformed input; every current caller maps both to the same fallback.
// On any rejection the out-param is left unchanged and the caller falls back.

inline std::string_view TrimAsciiWhitespace(std::string_view text) noexcept {
    std::size_t begin = 0u;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

template <typename T>
bool TryParseIntegralStrict(std::string_view text, T& out) noexcept {
    static_assert(std::is_integral_v<T>);
    text = TrimAsciiWhitespace(text);
    if (text.empty()) {
        return false;
    }

    T parsed{};
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = parsed;
    return true;
}

inline bool TryParseDoubleStrict(std::string_view text, double& out) noexcept {
    text = TrimAsciiWhitespace(text);
    if (text.empty()) {
        return false;
    }

    double parsed = 0.0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

}  // namespace svg_mb_control
