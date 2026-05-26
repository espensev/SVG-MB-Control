#pragma once

#include "control_config.h"
#include "control_loop.h"

#include <iosfwd>
#include <optional>

namespace svg_mb_control {

// Prints a curated, operator-facing summary of the loaded controller
// configuration. `loop` is optional because non-control-loop configs
// (e.g. read-loop only) need not declare the `control_loop` subtree.
// When `json` is true, emits a structured JSON object on a single
// indented document. Otherwise emits a grouped human-readable layout.
//
// The summary is intentionally a subset of the full config: it favors
// fields an operator inspects when tuning (cadence, write timing,
// smoothing, boost thresholds, low-band, channels). It is not a
// round-trippable dump of `control.json`.
void PrintControlConfigSummary(std::ostream& out,
                               const ControlConfig& base,
                               const std::optional<ControlLoopConfig>& loop,
                               bool json);

}  // namespace svg_mb_control
