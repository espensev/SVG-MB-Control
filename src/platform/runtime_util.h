#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace svg_mb_control {

// Returns true when `pid` refers to a process that is currently running.
// Returns false for pid 0 or any process that cannot be opened/queried.
bool IsProcessActive(std::uint32_t pid);

// Parses a local-time ISO-8601 timestamp ("%Y-%m-%dT%H:%M:%S"). Returns
// std::nullopt for empty or unparseable input. tm_isdst is set to -1 so the
// C runtime resolves daylight-saving for the parsed local time; this single
// semantic replaces two formerly divergent copies (one of which omitted the
// tm_isdst reset and so misinterpreted DST-period timestamps).
std::optional<std::chrono::system_clock::time_point> ParseLocalIso8601(
    std::string_view value);

// Formats a time point as "%Y-%m-%dT%H:%M:%S" in local time. Returns an
// empty string on conversion failure. Inverse of ParseLocalIso8601.
std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp);

}  // namespace svg_mb_control
