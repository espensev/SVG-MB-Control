#pragma once

#include "calibration.h"
#include "control_supervisor.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control {

// Parsed CLI state. Each field maps directly to a previously inline
// local in RunApp. Action shortcuts (help, version, diagnose-*) are
// flagged here and dispatched after parsing, so the parse loop has no
// side-effects beyond populating this struct.
struct CliOptions {
    std::filesystem::path config_path;
    bool config_path_explicit = false;
    std::string profile_name;
    std::string set_profile_name;
    bool set_profile_requested = false;
    bool foreground_launch = false;
    bool supervisor_launch = false;
    bool confirm_start = false;
    bool start_requested = false;
    bool status_requested = false;
    bool health_requested = false;
    bool service_probe_requested = false;
    bool show_config_requested = false;
    bool json_output_requested = false;
    bool stop_requested = false;
    bool restart_requested = false;
    bool reset_breakers_requested = false;
    std::optional<std::uint32_t> reset_breaker_channel;
    RunMode run_mode = RunMode::kOneShot;
    bool run_mode_explicit = false;
    std::uint32_t write_channel = 0u;
    bool write_channel_explicit = false;
    double write_pct = 0.0;
    bool write_pct_explicit = false;
    std::uint32_t write_hold_ms = 0u;
    bool write_hold_ms_explicit = false;
    std::optional<std::uint32_t> calibrate_channel;
    std::optional<std::uint32_t> calibrate_step_ms;
    std::optional<std::uint32_t> calibrate_cooldown_ms;
    std::optional<std::vector<CalibrationStepSpec>> calibrate_sequence;
    std::optional<std::uint32_t> calibrate_settle_window_ms;
    std::optional<double> calibrate_abort_temp_c;
    std::filesystem::path calibrate_output_path;
    bool help_requested = false;
    bool version_requested = false;
    bool diagnose_amd_requested = false;
    bool diagnose_gpu_requested = false;
};

// Parses argv into a CliOptions struct. Throws std::runtime_error with
// a user-facing message on unknown options, missing required values, or
// out-of-range values. Caller dispatches based on the populated flags.
CliOptions ParseCliOptions(int argc, wchar_t** argv);

// Writes the multi-line usage block to stdout.
void PrintUsage();

// Writes the version banner ("svg-mb-control X.Y.Z (commit)") to stdout.
void PrintVersion();

}  // namespace svg_mb_control
