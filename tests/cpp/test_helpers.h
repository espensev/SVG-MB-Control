#pragma once

// Shared assertion helpers and temp-name salt for the bespoke C++ tests
// (TS-1/TS-2,
// docs/archive/implemented-plans/testing-and-hotpath-simplification-review-2026-06-10.md).
// Each test executable is a single translation unit that includes this header:
// the inline g_failures is that TU's failure counter and main() returns
// nonzero when it is > 0. File-specific helpers (CSV field matchers,
// ExpectThrowsContaining, optional<double> matchers) stay in their own file
// and increment the same g_failures.
//
// ExpectNear NaN semantics, deliberately stricter than the historical
// per-file copies: NaN-vs-NaN passes (boost_stage golden traces compare NaN
// holds), NaN-vs-number FAILS. The old copies silently passed NaN-vs-number
// because `fabs(NaN - x) > tol` is false; no test in the suite relied on
// that (CTest 12/12 after the swap).

#include <cmath>
#include <iostream>
#include <random>
#include <string>

inline int g_failures = 0;

inline void ExpectTrue(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

inline void ExpectFalse(bool condition, const std::string& message) {
    ExpectTrue(!condition, message);
}

inline void ExpectNear(double actual, double expected, double tolerance,
                       const std::string& message) {
    if (std::isnan(actual) && std::isnan(expected)) {
        return;
    }
    if (std::isnan(actual) || std::isnan(expected) ||
        std::fabs(actual - expected) > tolerance) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected " << expected
                  << " got " << actual << '\n';
    }
}

inline void ExpectEqual(const std::string& actual, const std::string& expected,
                        const std::string& message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected [" << expected
                  << "] got [" << actual << "]\n";
    }
}

// A per-process-unique filename component so two concurrent test processes
// (for example two Test-LocalCI runs) never share a %TEMP% path. A per-process
// counter alone is insufficient: it resets to the same value in every process,
// so every process would reuse the same file name and one run could truncate
// the file mid-read in another. random_device yields a distinct salt per
// process; the counter disambiguates files within one process.
inline std::string UniqueTempSuffix() {
    static const unsigned long long kProcessSalt = [] {
        std::random_device rd;
        return (static_cast<unsigned long long>(rd()) << 32) ^
               static_cast<unsigned long long>(rd());
    }();
    static unsigned long long counter = 0;
    return std::to_string(kProcessSalt) + "_" + std::to_string(++counter);
}
