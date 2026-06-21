#pragma once

// FEAT-0023 (REQ-MPROFILE-01) machine-base + behavior-overlay composition.
//
// A control-config file may either be a complete control config (today's shape)
// or a composition descriptor with a top-level "compose" object:
//
//   { "compose": { "machine_base": "<policy.v1 path>",
//                  "behavior_overlay": "<overlay path>" } }
//
// The machine-base is a `svg_mb_control.machine_cooling_policy.v1` file: it owns
// the physical fan inventory and each controlled channel's minimum duty
// (`release_min_duty_pct`). The behavior-overlay is a control-config-shaped JSON
// carrying the tunable behavior (`control_loop` curves / boosts / cadence) with
// the per-channel `min_duty_pct` REMOVED. Composition injects the policy's
// minimum duty into each overlay channel, so the physical floor is defined once
// (in the machine base) instead of duplicated in every behavior file. The result
// parses through the existing LoadControlConfig / LoadControlLoopConfig.
//
// A file with no "compose" key is returned unchanged, so the no-catalog /
// single-`--config` path is byte-for-byte today's behavior.

#include <filesystem>

#include <nlohmann/json.hpp>

namespace svg_mb_control {

struct ComposedConfig {
    // The effective control-config JSON to parse (the file's own JSON when it is
    // not a composition descriptor; the merged JSON when it is).
    nlohmann::json root;
    // The path that relative config fields (snapshot_path / runtime_home_path /
    // runtime_policy_path) resolve against: the input path for a plain config,
    // or the behavior-overlay path for a composition descriptor (the overlay
    // owns those fields, so they resolve relative to the overlay's directory).
    std::filesystem::path base_path;
};

// Reads `config_path` and, if it is a composition descriptor, composes the
// machine-base + behavior-overlay into one control-config JSON. Throws
// std::runtime_error on a malformed compose block, an unreadable referenced
// file, an overlay missing control_loop.channels, or an overlay channel that is
// not a controlled channel in the machine base.
ComposedConfig ComposeConfigRoot(const std::filesystem::path& config_path);

}  // namespace svg_mb_control
