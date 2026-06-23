#include "app/app_args.h"

#include "calibration.h"
#include "control_supervisor.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef SVG_MB_CONTROL_VERSION
#define SVG_MB_CONTROL_VERSION "unknown"
#endif
#ifndef SVG_MB_CONTROL_GIT_HASH
#define SVG_MB_CONTROL_GIT_HASH "unknown"
#endif

namespace svg_mb_control {

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

std::uint32_t ParseUInt32Arg(const wchar_t* value, const char* flag_name) {
    const std::wstring text(value == nullptr ? L"" : value);
    if (text.empty()) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            throw std::runtime_error(
                std::string("Invalid ") + flag_name + " value.");
        }
    }
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed >
            static_cast<unsigned long long>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::out_of_range("uint32 overflow");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

double ParseDoubleArg(const wchar_t* value, const char* flag_name) {
    const std::wstring text(value == nullptr ? L"" : value);
    std::size_t parsed_chars = 0u;
    try {
        const double parsed = std::stod(text, &parsed_chars);
        if (parsed_chars != text.size() || !std::isfinite(parsed)) {
            throw std::invalid_argument("trailing characters or non-finite");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

std::string ParseAsciiStringArg(const wchar_t* value, const char* flag_name) {
    const std::wstring text(value == nullptr ? L"" : value);
    if (text.empty()) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch <= 0 || ch > 127) {
            throw std::runtime_error(
                std::string("Invalid ") + flag_name + " value.");
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::uint32_t ParseWriteChannel(const wchar_t* value) {
    return ParseUInt32Arg(value, "--write-channel");
}

double ParseWritePct(const wchar_t* value) {
    const double parsed = ParseDoubleArg(value, "--write-pct");
    if (parsed < 0.0 || parsed > 100.0) {
        throw std::runtime_error("Invalid --write-pct value.");
    }
    return parsed;
}

std::uint32_t ParseWriteHoldMs(const wchar_t* value) {
    return ParseUInt32Arg(value, "--write-hold-ms");
}

}  // namespace

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control [--start|--status|--health|--service-probe|--show-config|--stop|--restart|--reset-breakers] [--json] [--config <path>] [--profile <name>] [--set-profile <name>]\n"
        << "                 [--reset-breaker-channel <n>]\n"
        << "  svg-mb-control [--mode <one-shot|read-loop|write-once|control-loop|calibrate|evidence-log>] [--config <path>] "
           << "[--profile <name>] [--write-channel <n>] [--write-pct <pct>] [--write-hold-ms <ms>]\n"
        << "  svg-mb-control --mode calibrate [--calibrate-channel <n>] "
           << "[--calibrate-step-ms <ms>] [--calibrate-cooldown-ms <ms>] "
           << "[--calibrate-sequence <pct:ms[,pct:ms...]>] "
           << "[--calibrate-settle-window-ms <ms>] [--calibrate-abort-temp-c <c>] "
           << "[--calibrate-output <path>]\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "  svg-mb-control analyze prune [--runtime-home <path>] "
           << "[--db <path>] [--retain-days <days>] [--dry-run|--apply] [--quiet]\n"
        << "  svg-mb-control analyze report [--runtime-home <path>] "
           << "[--db <path>] [--run <id>|--session <ts>] [--idle-seconds <s>] "
           << "[--load-threshold-c <c>] [--gpu-load-threshold-c <c>] "
           << "[--json] [--out <path>] "
           << "[--manifest-out <path>] "
           << "[--decision-record-out <path|auto>|--no-decision-record] "
           << "[--profile <name>] [--hypothesis <text>] "
           << "[--decision <text>] [--notes <text>]\n"
        << "  svg-mb-control --diagnose-amd\n"
        << "  svg-mb-control --diagnose-gpu\n"
        << "  svg-mb-control --confirm-start\n"
        << "  svg-mb-control --help|-h\n"
        << "  svg-mb-control --version\n";
}

void PrintVersion() {
    std::cout << "svg-mb-control " << kVersion;
    if (std::string(kGitHash) != "unknown") {
        std::cout << " (" << kGitHash << ")";
    }
    std::cout << '\n';
}

CliOptions ParseCliOptions(int argc, wchar_t** argv) {
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        auto require_value = [&]() -> const wchar_t* {
            if (index + 1 >= argc) {
                throw std::runtime_error("Missing value for option.");
            }
            ++index;
            return argv[index];
        };

        if (arg == L"--config") {
            options.config_path = std::filesystem::path(require_value());
            options.config_path_explicit = true;
        } else if (arg == L"--profile") {
            options.profile_name =
                ParseAsciiStringArg(require_value(), "--profile");
        } else if (arg == L"--set-profile") {
            options.set_profile_name =
                ParseAsciiStringArg(require_value(), "--set-profile");
            options.set_profile_requested = true;
        } else if (arg == L"--run-foreground") {
            options.foreground_launch = true;
        } else if (arg == L"--run-supervisor") {
            options.supervisor_launch = true;
        } else if (arg == L"--start") {
            options.start_requested = true;
        } else if (arg == L"--status") {
            options.status_requested = true;
        } else if (arg == L"--health") {
            options.health_requested = true;
        } else if (arg == L"--service-probe") {
            options.service_probe_requested = true;
        } else if (arg == L"--show-config") {
            options.show_config_requested = true;
        } else if (arg == L"--json") {
            options.json_output_requested = true;
        } else if (arg == L"--stop") {
            options.stop_requested = true;
        } else if (arg == L"--restart") {
            options.restart_requested = true;
        } else if (arg == L"--reset-breakers") {
            options.reset_breakers_requested = true;
        } else if (arg == L"--reset-breaker-channel") {
            options.reset_breaker_channel = ParseUInt32Arg(
                require_value(), "--reset-breaker-channel");
            options.reset_breakers_requested = true;
        } else if (arg == L"--confirm-start") {
            options.confirm_start = true;
        } else if (arg == L"--mode") {
            options.run_mode = ParseRunMode(require_value());
            options.run_mode_explicit = true;
        } else if (arg == L"--write-channel") {
            options.write_channel = ParseWriteChannel(require_value());
            options.write_channel_explicit = true;
        } else if (arg == L"--write-pct") {
            options.write_pct = ParseWritePct(require_value());
            options.write_pct_explicit = true;
        } else if (arg == L"--write-hold-ms") {
            options.write_hold_ms = ParseWriteHoldMs(require_value());
            options.write_hold_ms_explicit = true;
        } else if (arg == L"--calibrate-channel") {
            options.calibrate_channel = ParseUInt32Arg(
                require_value(), "--calibrate-channel");
        } else if (arg == L"--calibrate-step-ms") {
            options.calibrate_step_ms = ParseUInt32Arg(
                require_value(), "--calibrate-step-ms");
        } else if (arg == L"--calibrate-cooldown-ms") {
            options.calibrate_cooldown_ms = ParseUInt32Arg(
                require_value(), "--calibrate-cooldown-ms");
        } else if (arg == L"--calibrate-sequence") {
            options.calibrate_sequence =
                ParseCalibrationSequence(require_value());
        } else if (arg == L"--calibrate-settle-window-ms") {
            options.calibrate_settle_window_ms = ParseUInt32Arg(
                require_value(), "--calibrate-settle-window-ms");
        } else if (arg == L"--calibrate-abort-temp-c") {
            options.calibrate_abort_temp_c = ParseDoubleArg(
                require_value(), "--calibrate-abort-temp-c");
        } else if (arg == L"--calibrate-output") {
            options.calibrate_output_path =
                std::filesystem::path(require_value());
        } else if (arg == L"--help" || arg == L"-h") {
            options.help_requested = true;
        } else if (arg == L"--version") {
            options.version_requested = true;
        } else if (arg == L"--bridge-exe-path" ||
                   arg == L"--bench-exe-path" ||
                   arg == L"--bridge-command" ||
                   arg == L"--duration-ms" ||
                   arg == L"--timeout-ms") {
            throw std::runtime_error(
                "Legacy bridge options were removed. This branch runs direct-only.");
        } else if (arg == L"--diagnose-amd") {
            options.diagnose_amd_requested = true;
        } else if (arg == L"--diagnose-gpu") {
            options.diagnose_gpu_requested = true;
        } else {
            throw std::runtime_error("Unknown option.");
        }
    }
    return options;
}

}  // namespace svg_mb_control
