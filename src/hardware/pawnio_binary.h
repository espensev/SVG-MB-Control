#pragma once

#include "windows_lean.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

enum class PawnIoStatus {
    ok = 0,
    error = -1,
    invalid_arg = -2,
    not_supported = -4,
    not_found = -5,
    access_denied = -6,
    hash_mismatch = -7,
};

const char* PawnIoStatusString(PawnIoStatus status);

enum class PawnIoVerification {
    // Mismatch is recorded in out_warning and the bin is still loaded. Used
    // for the legacy SMN-only bin so an existing install with a locally
    // swapped AMDFamily17.bin keeps working on upgrade.
    warn_only,
    // Mismatch returns hash_mismatch and refuses the load. Required for any
    // bin that exposes MSR reads: a silently swapped bin could lie about
    // counter contents.
    strict,
    // Hash is not computed. Reserved for the SVG_MB_CONTROL_PAWNIO_SKIP_HASH=1
    // escape hatch; do not use as a static default.
    skip,
};

struct PawnIoBinarySpec {
    std::string_view filename;
    std::string_view expected_sha256_hex;
    std::string_view display_name;
    PawnIoVerification verification = PawnIoVerification::warn_only;
};

extern const PawnIoBinarySpec kPawnIoSpecAmdFamily17V1;

std::filesystem::path ResolvePawnIoBinaryPath(const PawnIoBinarySpec& spec);

// out_hash_mismatch (optional): set to true when the bin's SHA-256 does not
// match spec.expected_sha256_hex, even under warn_only (where the load still
// succeeds). Lets a caller field-gate MSR-derived values on an unverified bin
// without string-matching out_warning (FEAT-0006 §9 counter-read safety review).
PawnIoStatus LoadPawnIoBinary(HANDLE device_handle,
                              const PawnIoBinarySpec& spec,
                              const std::filesystem::path& bin_path,
                              std::string* out_warning,
                              bool* out_hash_mismatch = nullptr);

bool ComputeSha256Hex(const std::uint8_t* data,
                      std::size_t size,
                      std::string* out_hex);

}  // namespace svg_mb_control
