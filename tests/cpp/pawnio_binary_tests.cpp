#include "pawnio_binary.h"

#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

// Test vectors from FIPS 180-4 / RFC 6234.
constexpr const char* kSha256EmptyHex =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kSha256AbcHex =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

void TestSha256OfEmpty() {
    std::string hex;
    const bool ok = svg_mb_control::ComputeSha256Hex(nullptr, 0u, &hex);
    ExpectTrue(ok, "ComputeSha256Hex returns true for empty input");
    ExpectEqual(hex, kSha256EmptyHex, "SHA-256 of empty matches FIPS vector");
}

void TestSha256OfAbc() {
    const std::uint8_t data[] = {'a', 'b', 'c'};
    std::string hex;
    const bool ok = svg_mb_control::ComputeSha256Hex(
        data, sizeof(data), &hex);
    ExpectTrue(ok, "ComputeSha256Hex returns true for 'abc'");
    ExpectEqual(hex, kSha256AbcHex, "SHA-256 of 'abc' matches FIPS vector");
}

void TestSha256HexIsLowercase() {
    const std::uint8_t data[] = {0xFFu, 0x00u, 0xAAu};
    std::string hex;
    svg_mb_control::ComputeSha256Hex(data, sizeof(data), &hex);
    const bool all_lower = std::all_of(hex.begin(), hex.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    ExpectTrue(all_lower, "SHA-256 hex output uses lowercase digits");
}

void TestSha256RejectsNullBufferOrOutput() {
    std::string hex;
    const std::uint8_t data[] = {0x01u};
    ExpectTrue(!svg_mb_control::ComputeSha256Hex(data, sizeof(data), nullptr),
               "ComputeSha256Hex rejects null out_hex");
    ExpectTrue(!svg_mb_control::ComputeSha256Hex(nullptr, 1u, &hex),
               "ComputeSha256Hex rejects null data with non-zero size");
}

void TestAmdFamily17V1SpecPopulated() {
    const auto& spec = svg_mb_control::kPawnIoSpecAmdFamily17V1;
    ExpectEqual(std::string(spec.filename), "AMDFamily17.bin",
                "v1 spec names the shipped bin");
    ExpectTrue(spec.expected_sha256_hex.size() == 64u,
               "v1 spec carries a 64-char sha256 hex string");
    ExpectTrue(spec.verification == svg_mb_control::PawnIoVerification::warn_only,
               "v1 spec defaults to warn_only verification");
}

void TestResolveReturnsEmptyForUnknownFilename() {
    const svg_mb_control::PawnIoBinarySpec bogus{
        "definitely-not-a-real-pawnio-binary-xyz123.bin",
        "",
        "test-bogus",
        svg_mb_control::PawnIoVerification::skip,
    };
    const auto path = svg_mb_control::ResolvePawnIoBinaryPath(bogus);
    ExpectTrue(path.empty(),
               "ResolvePawnIoBinaryPath returns empty for unknown filename");
}

void TestStatusStringCoversAllVariants() {
    using svg_mb_control::PawnIoStatus;
    using svg_mb_control::PawnIoStatusString;
    constexpr PawnIoStatus all[] = {
        PawnIoStatus::ok,           PawnIoStatus::error,
        PawnIoStatus::invalid_arg,  PawnIoStatus::not_supported,
        PawnIoStatus::not_found,    PawnIoStatus::access_denied,
        PawnIoStatus::hash_mismatch,
    };
    for (PawnIoStatus s : all) {
        const char* text = PawnIoStatusString(s);
        ExpectTrue(text != nullptr && std::strcmp(text, "unknown") != 0,
                   "PawnIoStatusString maps every named variant");
    }
}

}  // namespace

int main() {
    TestSha256OfEmpty();
    TestSha256OfAbc();
    TestSha256HexIsLowercase();
    TestSha256RejectsNullBufferOrOutput();
    TestAmdFamily17V1SpecPopulated();
    TestResolveReturnsEmptyForUnknownFilename();
    TestStatusStringCoversAllVariants();
    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "all pawnio_binary tests passed\n";
    return 0;
}
