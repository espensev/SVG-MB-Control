#include "amd_reader.h"
#include "analyze/analyze_ingest.h"
#include "analyze/analyze_prune.h"
#include "calibration.h"
#include "control_config.h"
#include "control_loop.h"
#include "control_supervisor.h"
#include "direct_runtime_snapshot.h"
#include "evidence_log.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "read_loop.h"
#include "runtime_artifacts.h"
#include "runtime_health.h"
#include "runtime_lifecycle.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"
#include "service_probe.h"
#include "write_orchestrator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// The supervisor/launcher subsystem and RunMode now live in
// control_supervisor.{h,cpp}. wmain is at global scope, so these names are
// pulled in here rather than into the anonymous namespace below.
using svg_mb_control::ConfirmDetachedLaunch;
using svg_mb_control::IsLongRunningMode;
using svg_mb_control::LaunchDetachedLongRunningMode;
using svg_mb_control::ParseRunMode;
using svg_mb_control::PrintRuntimeHealth;
using svg_mb_control::PrintRuntimeStatus;
using svg_mb_control::RequestStopAndWait;
using svg_mb_control::RunMode;
using svg_mb_control::RunSupervisedLongRunningMode;

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

svg_mb_control::ReadLoop* g_active_read_loop = nullptr;
std::atomic<bool> g_stop_signaled{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop_signaled.store(true);
            if (g_active_read_loop != nullptr) {
                g_active_read_loop->RequestStop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control [--start|--status|--health|--stop|--restart] [--json] [--config <path>]\n"
        << "  svg-mb-control [--mode <one-shot|read-loop|write-once|control-loop|calibrate|evidence-log>] [--config <path>] "
           << "[--write-channel <n>] [--write-pct <pct>] [--write-hold-ms <ms>]\n"
        << "  svg-mb-control --mode calibrate [--calibrate-channel <n>] "
           << "[--calibrate-step-ms <ms>] [--calibrate-cooldown-ms <ms>] "
           << "[--calibrate-sequence <pct:ms[,pct:ms...]>] "
           << "[--calibrate-settle-window-ms <ms>] [--calibrate-abort-temp-c <c>] "
           << "[--calibrate-output <path>]\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "  svg-mb-control analyze prune [--runtime-home <path>] "
           << "[--db <path>] [--retain-days <days>] [--dry-run|--apply] [--quiet]\n"
        << "  svg-mb-control --diagnose-amd\n"
        << "  svg-mb-control --diagnose-gpu\n"
        << "  svg-mb-control --confirm-start\n"
        << "  svg-mb-control --help|-h\n"
        << "  svg-mb-control --version\n";
}

void PrintAnalyzeUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "    Reads CSV archives, manifests, events.jsonl and "
           << "plant_model.json from the\n"
        << "    runtime home and ingests them into a sqlite database. "
           << "Default db path is\n"
        << "    <runtime-home>/svg_mb_control.db. Idempotent on "
           << "previously-seen artifacts\n"
        << "    unless --force is passed.\n";
    std::cout
        << "  svg-mb-control analyze prune [--runtime-home <path>] "
           << "[--db <path>] [--retain-days <days>] [--dry-run|--apply] [--quiet]\n"
        << "    Finds old archive CSV/manifest bundles. Dry-run is the default; "
           << "--apply is\n"
        << "    required before files are deleted. Deletion is gated on the run "
           << "already\n"
        << "    being present in the sqlite ingest database.\n";
}

bool ResolveAnalyzeRuntimeHome(
    std::filesystem::path& runtime_home,
    std::filesystem::path config_path,
    bool config_path_explicit,
    std::optional<svg_mb_control::ControlConfig>& resolved_config) {
    if (config_path.empty()) {
        config_path = svg_mb_control::GetEnvironmentPath(
            L"SVG_MB_CONTROL_CONFIG");
    }
    if (config_path.empty()) {
        config_path = svg_mb_control::ResolveDefaultControlConfigPath();
    }

    if (!config_path.empty()) {
        const std::filesystem::path absolute_config_path =
            std::filesystem::absolute(config_path).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(absolute_config_path, ec)) {
            try {
                resolved_config = svg_mb_control::LoadControlConfig(
                    absolute_config_path);
            } catch (const std::exception&) {
                resolved_config.reset();
            }
        } else if (config_path_explicit) {
            std::cerr << "Error: control config not found: "
                      << absolute_config_path.string() << '\n';
            return false;
        }
    }

    if (runtime_home.empty()) {
        runtime_home = resolved_config.has_value()
            ? svg_mb_control::ResolveRuntimeHomePath(*resolved_config)
            : svg_mb_control::ResolveRuntimeHomePath(
                  svg_mb_control::ControlConfig{});
    }
    return true;
}

bool ParseUint32Option(const wchar_t* text, std::uint32_t& out) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(text));
        if (parsed > UINT32_MAX) {
            return false;
        }
        out = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

int RunAnalyzeCommand(int argc, wchar_t** argv) {
    if (argc < 3) {
        PrintAnalyzeUsage();
        return 1;
    }
    const std::wstring verb = argv[2];
    if (verb == L"--help" || verb == L"-h") {
        PrintAnalyzeUsage();
        return 0;
    }
    if (verb != L"ingest" && verb != L"prune") {
        std::cerr << "Error: unknown analyze subcommand. Try "
                  << "'svg-mb-control analyze --help'.\n";
        return 1;
    }

    svg_mb_control::analyze::IngestOptions options;
    svg_mb_control::analyze::PruneOptions prune_options;
    std::filesystem::path config_path;
    bool config_path_explicit = false;
    bool retain_days_explicit = false;

    auto require_value = [&](int& index) -> const wchar_t* {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for option.");
        }
        ++index;
        return argv[index];
    };

    for (int index = 3; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--runtime-home") {
            options.runtime_home = std::filesystem::path(require_value(index));
            prune_options.runtime_home = options.runtime_home;
        } else if (arg == L"--db") {
            options.db_path = std::filesystem::path(require_value(index));
            prune_options.db_path = options.db_path;
        } else if (arg == L"--config") {
            config_path = std::filesystem::path(require_value(index));
            config_path_explicit = true;
        } else if (arg == L"--force") {
            if (verb != L"ingest") {
                std::cerr << "Error: --force is only valid for analyze ingest.\n";
                PrintAnalyzeUsage();
                return 1;
            }
            options.force = true;
        } else if (arg == L"--retain-days") {
            if (verb != L"prune") {
                std::cerr << "Error: --retain-days is only valid for analyze prune.\n";
                PrintAnalyzeUsage();
                return 1;
            }
            std::uint32_t retain_days = 0u;
            if (!ParseUint32Option(require_value(index), retain_days)) {
                std::cerr << "Error: invalid --retain-days value.\n";
                return 1;
            }
            prune_options.retain_days = retain_days;
            retain_days_explicit = true;
        } else if (arg == L"--apply") {
            if (verb != L"prune") {
                std::cerr << "Error: --apply is only valid for analyze prune.\n";
                PrintAnalyzeUsage();
                return 1;
            }
            prune_options.apply = true;
        } else if (arg == L"--dry-run") {
            if (verb != L"prune") {
                std::cerr << "Error: --dry-run is only valid for analyze prune.\n";
                PrintAnalyzeUsage();
                return 1;
            }
            prune_options.apply = false;
        } else if (arg == L"--quiet") {
            options.quiet = true;
            prune_options.quiet = true;
        } else if (arg == L"--help" || arg == L"-h") {
            PrintAnalyzeUsage();
            return 0;
        } else {
            std::cerr << "Error: unknown analyze ingest option.\n";
            PrintAnalyzeUsage();
            return 1;
        }
    }

    std::optional<svg_mb_control::ControlConfig> config;
    std::filesystem::path runtime_home =
        verb == L"prune" ? prune_options.runtime_home : options.runtime_home;
    if (!ResolveAnalyzeRuntimeHome(runtime_home, config_path,
                                   config_path_explicit, config)) {
        return 1;
    }
    options.runtime_home = runtime_home;
    prune_options.runtime_home = runtime_home;

    if (verb == L"prune") {
        if (!retain_days_explicit && config.has_value()) {
            prune_options.retain_days = config->log_retain_days;
        }
        return svg_mb_control::analyze::RunAnalyzePrune(prune_options);
    }

    return svg_mb_control::analyze::RunAnalyzeIngest(options);
}

void PrintVersion() {
    std::cout << "svg-mb-control " << kVersion;
    if (std::string(kGitHash) != "unknown") {
        std::cout << " (" << kGitHash << ")";
    }
    std::cout << '\n';
}

void PrintBlockedChannels(const std::vector<std::uint32_t>& channels) {
    if (channels.empty()) {
        std::cout << "(none)";
        return;
    }

    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        std::cout << channels[index];
    }
}

void PrintCommonLoopStartup(const char* mode,
                            const svg_mb_control::ControlConfig& config,
                            const std::filesystem::path& runtime_home,
                            const svg_mb_control::RuntimeWritePolicy& policy) {
    std::cout << "svg-mb-control: starting " << mode << '\n'
              << "  pid: " << GetCurrentProcessId() << '\n'
              << "  config: " << config.source_path.string() << '\n'
              << "  runtime_home: " << runtime_home.string() << '\n'
              << "  status: " << (runtime_home / "control_runtime.json").string()
              << '\n'
              << "  events: "
              << (runtime_home / "logs" / "svg_mb_control_events.jsonl").string()
              << '\n'
              << "  policy: "
              << (policy.present ? policy.source_path.string()
                                 : std::string("(none)"))
              << '\n'
              << "  writes_enabled: "
              << (policy.writes_enabled ? "true" : "false") << '\n'
              << "  blocked_channels: ";
    PrintBlockedChannels(policy.blocked_channels);
    std::cout << '\n';
}

void PrintControlLoopStartup(
    const svg_mb_control::ControlConfig& config,
    const svg_mb_control::ControlLoopConfig& loop_config,
    const std::filesystem::path& runtime_home,
    const svg_mb_control::RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("control-loop", config, runtime_home, policy);
    std::cout << "  poll_tick_ms: " << loop_config.poll_tick_ms << '\n'
              << "  write_cooldown_ms: " << loop_config.write_cooldown_ms << '\n'
              << "  deadband_pct: " << loop_config.deadband_pct << '\n'
              << "  hold_ms: " << loop_config.control_hold_ms << '\n'
              << "  controlled_channels:\n";
    for (const auto& channel : loop_config.channels) {
        std::cout << "    channel " << channel.channel
                  << ": floor=" << channel.min_duty_pct
                  << "% blend="
                  << svg_mb_control::TempBlendToString(channel.temp_blend)
                  << " shape="
                  << svg_mb_control::CurveShapeToString(channel.curve_shape)
                  << " rise_rate=" << channel.rise_rate_pct_per_min
                  << "%/min fall_rate=" << channel.fall_rate_pct_per_min
                  << "%/min"
                  << " demand_alpha="
                  << channel.demand_smoothing_rise_alpha
                  << "/" << channel.demand_smoothing_fall_alpha
                  << " decay_latch="
                  << channel.decay_latch_above_pct
                  << "%/" << channel.decay_latch_pct_per_min
                  << "%/min"
                  << " thermal_pressure="
                  << channel.thermal_pressure_start_c
                  << '-' << channel.thermal_pressure_full_c
                  << "C +" << channel.thermal_pressure_max_boost_pct
                  << "% @ " << channel.thermal_pressure_rise_pct_per_sec
                  << "%/s -" << channel.thermal_pressure_fall_pct_per_sec
                  << "%/s";
        if (channel.cpu_low_soak_max_boost_pct > 0.0) {
            std::cout << " cpu_low_soak="
                      << channel.cpu_low_soak_start_c
                      << '-' << channel.cpu_low_soak_full_c
                      << "C release<=" << channel.cpu_low_soak_release_c
                      << "C +" << channel.cpu_low_soak_max_boost_pct
                      << "% @ " << channel.cpu_low_soak_rise_pct_per_min
                      << "%/min -" << channel.cpu_low_soak_fall_pct_per_min
                      << "%/min";
        }
        std::cout << " curve=";
        for (std::size_t index = 0; index < channel.curve.size(); ++index) {
            if (index > 0) {
                std::cout << ',';
            }
            std::cout << channel.curve[index].temp_c << "C:"
                      << channel.curve[index].duty_pct << '%';
        }
        if (!channel.cpu_override_curve.empty()) {
            std::cout << " cpu_override_curve=";
            for (std::size_t index = 0;
                 index < channel.cpu_override_curve.size(); ++index) {
                if (index > 0) {
                    std::cout << ',';
                }
                std::cout << channel.cpu_override_curve[index].temp_c << "C:"
                          << channel.cpu_override_curve[index].duty_pct << '%';
            }
        }
        std::cout << '\n';
    }
    std::cout << std::flush;
}

void PrintReadLoopStartup(const svg_mb_control::ControlConfig& config,
                          const std::filesystem::path& runtime_home,
                          const svg_mb_control::RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("read-loop", config, runtime_home, policy);
    std::cout << "  poll_ms: " << config.poll_ms << '\n' << std::flush;
}

void PrintEvidenceLogStartup(const svg_mb_control::ControlConfig& config,
                             const std::filesystem::path& runtime_home,
                             const svg_mb_control::RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("evidence-log", config, runtime_home, policy);
    std::cout << "  poll_ms: " << config.poll_ms << '\n'
              << "  evidence_gpu_sample_mode: "
              << config.evidence_gpu_sample_mode << '\n'
              << std::flush;
}

std::uint32_t ParseWriteChannel(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-channel value.");
    }
}

double ParseWritePct(const wchar_t* value) {
    try {
        return std::stod(std::wstring(value));
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-pct value.");
    }
}

std::uint32_t ParseWriteHoldMs(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-hold-ms value.");
    }
}

std::uint32_t ParseUInt32Arg(const wchar_t* value, const char* flag_name) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

double ParseDoubleArg(const wchar_t* value, const char* flag_name) {
    try {
        return std::stod(std::wstring(value));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

std::vector<svg_mb_control::CalibrationStepSpec> ParseCalibrationSequence(
    const wchar_t* value) {
    const std::wstring input(value);
    if (input.empty()) {
        throw std::runtime_error("--calibrate-sequence cannot be empty.");
    }

    std::vector<svg_mb_control::CalibrationStepSpec> sequence;
    std::size_t offset = 0u;
    while (offset < input.size()) {
        const std::size_t comma = input.find(L',', offset);
        const std::size_t end =
            comma == std::wstring::npos ? input.size() : comma;
        const std::wstring token = input.substr(offset, end - offset);
        const std::size_t colon = token.find(L':');
        if (token.empty() || colon == std::wstring::npos ||
            colon == 0u || colon + 1u >= token.size()) {
            throw std::runtime_error(
                "Invalid --calibrate-sequence entry; expected duty_pct:hold_ms.");
        }

        double duty_pct = 0.0;
        std::uint32_t hold_ms = 0u;
        try {
            duty_pct = std::stod(token.substr(0u, colon));
            const unsigned long parsed_hold =
                std::stoul(token.substr(colon + 1u));
            hold_ms = static_cast<std::uint32_t>(parsed_hold);
        } catch (const std::exception&) {
            throw std::runtime_error(
                "Invalid --calibrate-sequence entry; expected duty_pct:hold_ms.");
        }
        if (duty_pct < 0.0 || duty_pct > 100.0) {
            throw std::runtime_error(
                "--calibrate-sequence duty_pct must be in [0, 100].");
        }
        if (hold_ms == 0u) {
            throw std::runtime_error(
                "--calibrate-sequence hold_ms must be greater than zero.");
        }
        sequence.push_back({duty_pct, hold_ms});

        if (comma == std::wstring::npos) {
            break;
        }
        offset = comma + 1u;
    }

    if (sequence.empty()) {
        throw std::runtime_error("--calibrate-sequence cannot be empty.");
    }
    return sequence;
}

std::string SampleDirectSnapshotJson(
    const svg_mb_control::ControlConfig* config) {
    const svg_mb_control::RuntimeWritePolicy runtime_policy =
        svg_mb_control::ResolveRuntimeWritePolicy(config);
    std::unique_ptr<svg_mb_control::FanWriter> writer =
        svg_mb_control::CreateFanWriter(runtime_policy);
    svg_mb_control::AmdReader amd_reader;
    svg_mb_control::GpuReader gpu_reader;
    const svg_mb_control::RuntimeSnapshot snapshot =
        svg_mb_control::SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *writer, runtime_policy);
    return svg_mb_control::SerializeRuntimeSnapshotJson(snapshot);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"analyze") {
            return RunAnalyzeCommand(argc, argv);
        }
        const bool no_launch_args = argc == 1;
        std::filesystem::path config_path;
        bool config_path_explicit = false;
        bool foreground_launch = false;
        bool supervisor_launch = false;
        bool confirm_start = false;
        bool start_requested = false;
        bool status_requested = false;
        bool health_requested = false;
        bool service_probe_requested = false;
        bool json_output_requested = false;
        bool stop_requested = false;
        bool restart_requested = false;
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
        std::optional<std::vector<svg_mb_control::CalibrationStepSpec>>
            calibrate_sequence;
        std::optional<std::uint32_t> calibrate_settle_window_ms;
        std::optional<double> calibrate_abort_temp_c;
        std::filesystem::path calibrate_output_path;

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
                config_path = std::filesystem::path(require_value());
                config_path_explicit = true;
            } else if (arg == L"--run-foreground") {
                foreground_launch = true;
            } else if (arg == L"--run-supervisor") {
                supervisor_launch = true;
            } else if (arg == L"--start") {
                start_requested = true;
            } else if (arg == L"--status") {
                status_requested = true;
            } else if (arg == L"--health") {
                health_requested = true;
            } else if (arg == L"--service-probe") {
                service_probe_requested = true;
            } else if (arg == L"--json") {
                json_output_requested = true;
            } else if (arg == L"--stop") {
                stop_requested = true;
            } else if (arg == L"--restart") {
                restart_requested = true;
            } else if (arg == L"--confirm-start") {
                confirm_start = true;
            } else if (arg == L"--mode") {
                run_mode = ParseRunMode(require_value());
                run_mode_explicit = true;
            } else if (arg == L"--write-channel") {
                write_channel = ParseWriteChannel(require_value());
                write_channel_explicit = true;
            } else if (arg == L"--write-pct") {
                write_pct = ParseWritePct(require_value());
                write_pct_explicit = true;
            } else if (arg == L"--write-hold-ms") {
                write_hold_ms = ParseWriteHoldMs(require_value());
                write_hold_ms_explicit = true;
            } else if (arg == L"--calibrate-channel") {
                calibrate_channel = ParseUInt32Arg(
                    require_value(), "--calibrate-channel");
            } else if (arg == L"--calibrate-step-ms") {
                calibrate_step_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-step-ms");
            } else if (arg == L"--calibrate-cooldown-ms") {
                calibrate_cooldown_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-cooldown-ms");
            } else if (arg == L"--calibrate-sequence") {
                calibrate_sequence = ParseCalibrationSequence(require_value());
            } else if (arg == L"--calibrate-settle-window-ms") {
                calibrate_settle_window_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-settle-window-ms");
            } else if (arg == L"--calibrate-abort-temp-c") {
                calibrate_abort_temp_c = ParseDoubleArg(
                    require_value(), "--calibrate-abort-temp-c");
            } else if (arg == L"--calibrate-output") {
                calibrate_output_path =
                    std::filesystem::path(require_value());
            } else if (arg == L"--help" || arg == L"-h") {
                PrintUsage();
                return 0;
            } else if (arg == L"--version") {
                PrintVersion();
                return 0;
            } else if (arg == L"--bridge-exe-path" ||
                       arg == L"--bench-exe-path" ||
                       arg == L"--bridge-command" ||
                       arg == L"--duration-ms" ||
                       arg == L"--timeout-ms") {
                throw std::runtime_error(
                    "Legacy bridge options were removed. This branch runs direct-only.");
            } else if (arg == L"--diagnose-amd") {
                svg_mb_control::AmdReader reader;
                std::cout << "amd_reader.available: "
                          << (reader.available() ? "true" : "false") << '\n';
                std::cout << "amd_reader.init_warning: \""
                          << reader.init_warning() << "\"\n";
                const auto snapshot = reader.Sample();
                std::cout << "sample.available: "
                          << (snapshot.available ? "true" : "false") << '\n';
                std::cout << "sample.cpu_name: \"" << snapshot.cpu_name << "\"\n";
                std::cout << "sample.transport_path: \""
                          << snapshot.transport_path << "\"\n";
                std::cout << "sample.last_warning: \""
                          << snapshot.last_warning << "\"\n";
                std::cout << "sample.count: " << snapshot.samples.size() << '\n';
                for (std::size_t sample_index = 0u;
                     sample_index < snapshot.samples.size();
                     ++sample_index) {
                    const auto& sample = snapshot.samples[sample_index];
                    std::cout << "sample[" << sample_index << "].label: \""
                              << sample.label << "\"\n";
                    std::cout << "sample[" << sample_index
                              << "].temperature_c: " << sample.temperature_c
                              << '\n';
                }
                return snapshot.available ? 0 : 1;
            } else if (arg == L"--diagnose-gpu") {
                svg_mb_control::GpuReader reader;
                std::cout << "gpu_reader.available: "
                          << (reader.available() ? "true" : "false") << '\n';
                std::cout << "gpu_reader.init_warning: \""
                          << reader.init_warning() << "\"\n";
                const auto sample = reader.Sample();
                std::cout << "sample.available: "
                          << (sample.available ? "true" : "false") << '\n';
                std::cout << "sample.gpu_name: \"" << sample.gpu_name << "\"\n";
                std::cout << "sample.core_c: " << sample.core_c << '\n';
                std::cout << "sample.memjn_c: " << sample.memjn_c << '\n';
                std::cout << "sample.hotspot_c: " << sample.hotspot_c << '\n';
                std::cout << "sample.last_warning: \""
                          << sample.last_warning << "\"\n";
                return sample.available ? 0 : 1;
            } else {
                throw std::runtime_error("Unknown option.");
            }
        }

        if (config_path.empty()) {
            config_path = svg_mb_control::GetEnvironmentPath(L"SVG_MB_CONTROL_CONFIG");
            if (!config_path.empty()) {
                config_path_explicit = true;
            }
        }
        if (config_path.empty()) {
            config_path = svg_mb_control::ResolveDefaultControlConfigPath();
        }

        std::optional<svg_mb_control::ControlConfig> config;
        if (!config_path.empty()) {
            const std::filesystem::path absolute_config_path =
                std::filesystem::absolute(config_path).lexically_normal();
            if (!std::filesystem::exists(absolute_config_path)) {
                if (config_path_explicit) {
                    throw std::runtime_error("Control config not found: " +
                                             absolute_config_path.string());
                }
            } else {
                config = svg_mb_control::LoadControlConfig(absolute_config_path);
            }
        }

        if (!run_mode_explicit && config.has_value() &&
            !config->default_mode.empty()) {
            run_mode = ParseRunMode(config->default_mode);
        }

        const svg_mb_control::ControlConfig status_config =
            config.has_value() ? *config : svg_mb_control::ControlConfig{};
        const std::filesystem::path command_runtime_home =
            svg_mb_control::ResolveRuntimeHomePath(status_config);
        const std::uint32_t health_stale_after_ms =
            status_config.staleness_threshold_ms > 0u
                ? status_config.staleness_threshold_ms
                : 10000u;

        if (json_output_requested && !status_requested && !health_requested &&
            !service_probe_requested) {
            throw std::runtime_error(
                "--json requires --status, --health, or --service-probe.");
        }

        if (service_probe_requested) {
            return svg_mb_control::RunServiceProbe(
                command_runtime_home,
                config.has_value() ? &*config : nullptr,
                json_output_requested);
        }

        if (health_requested || (status_requested && json_output_requested)) {
            return PrintRuntimeHealth(command_runtime_home,
                                      json_output_requested,
                                      health_stale_after_ms);
        }

        if (status_requested) {
            return PrintRuntimeStatus(command_runtime_home);
        }

        if (stop_requested && !restart_requested) {
            return RequestStopAndWait(command_runtime_home);
        }

        if (restart_requested) {
            const int stop_result =
                RequestStopAndWait(command_runtime_home, true);
            if (stop_result != 0) {
                return stop_result;
            }
            start_requested = true;
        }

        if (supervisor_launch) {
            if (!config.has_value()) {
                throw std::runtime_error(
                    "--run-supervisor requires a control config.");
            }
            if (!IsLongRunningMode(run_mode)) {
                throw std::runtime_error(
                    "--run-supervisor requires read-loop or control-loop.");
            }
            return RunSupervisedLongRunningMode(run_mode, *config);
        }

        if (!foreground_launch && config.has_value() &&
            IsLongRunningMode(run_mode) &&
            (no_launch_args || confirm_start || start_requested)) {
            if (confirm_start && !ConfirmDetachedLaunch(run_mode, *config)) {
                std::cout << "svg-mb-control: start cancelled\n";
                return 0;
            }
            return LaunchDetachedLongRunningMode(run_mode, *config);
        }

        if (start_requested || restart_requested) {
            throw std::runtime_error(
                "--start/--restart requires a control config whose mode is read-loop or control-loop.");
        }

        if (config.has_value() && !config->runtime_policy_path.empty()) {
            const DWORD existing = GetEnvironmentVariableW(
                L"SVG_MB_RUNTIME_POLICY", nullptr, 0);
            if (existing == 0) {
                SetEnvironmentVariableW(
                    L"SVG_MB_RUNTIME_POLICY",
                    config->runtime_policy_path.wstring().c_str());
            }
        }

        const std::uint32_t reconcile_timeout_ms =
            config.has_value() ? config->restore_timeout_ms : 5000u;
        const std::filesystem::path reconcile_runtime_home =
            config.has_value()
                ? svg_mb_control::ResolveRuntimeHomePath(*config)
                : svg_mb_control::ResolveRuntimeHomePath(
                      svg_mb_control::ControlConfig{});
        const int reconcile_result = svg_mb_control::ReconcilePendingWrites(
            reconcile_runtime_home,
            svg_mb_control::ResolveRuntimeWritePolicy(
                config.has_value() ? &*config : nullptr),
            reconcile_timeout_ms);
        if (reconcile_result != 0) {
            std::cerr << "Error: pending writes reconciliation failed. "
                      << "Refusing to proceed." << '\n';
            return reconcile_result;
        }

        if (run_mode == RunMode::kWriteOnce) {
            if (!config.has_value()) {
                svg_mb_control::ControlConfig defaults;
                config = defaults;
            }
            if (!write_channel_explicit && !config->write_channel_set) {
                throw std::runtime_error("--mode write-once requires --write-channel or write_channel in config.");
            }
            if (!write_pct_explicit && !config->write_target_pct_set) {
                throw std::runtime_error("--mode write-once requires --write-pct or write_target_pct in config.");
            }
            if (!write_hold_ms_explicit && !config->write_hold_ms_set) {
                throw std::runtime_error("--mode write-once requires --write-hold-ms or write_hold_ms in config.");
            }
            svg_mb_control::WriteRequest request;
            request.channel = write_channel_explicit
                ? write_channel : config->write_channel;
            request.target_pct = write_pct_explicit
                ? write_pct : config->write_target_pct;
            request.hold_ms = write_hold_ms_explicit
                ? write_hold_ms : config->write_hold_ms;

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const int result = svg_mb_control::RunWriteOnce(
                *config, reconcile_runtime_home, request, g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kControlLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode control-loop requires a control config.");
            }
            if (config_path.empty()) {
                throw std::runtime_error("--mode control-loop requires a resolvable config path.");
            }
            const svg_mb_control::ControlLoopConfig loop_config =
                svg_mb_control::LoadControlLoopConfig(
                    std::filesystem::absolute(config_path).lexically_normal());
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintControlLoopStartup(
                *config, loop_config, reconcile_runtime_home, runtime_policy);

            svg_mb_control::ControlLoop control_loop(
                *config, loop_config, reconcile_runtime_home);

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const int result = control_loop.RunUntilStopped(g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kCalibrate) {
            if (calibrate_sequence.has_value() &&
                (calibrate_step_ms.has_value() ||
                 calibrate_cooldown_ms.has_value())) {
                throw std::runtime_error(
                    "--calibrate-sequence cannot be combined with "
                    "--calibrate-step-ms or --calibrate-cooldown-ms.");
            }
            svg_mb_control::CalibrationOptions options =
                svg_mb_control::DefaultCalibrationOptions();
            if (calibrate_sequence.has_value()) {
                options.sequence = *calibrate_sequence;
            }
            if (calibrate_step_ms.has_value()) {
                for (auto& step : options.sequence) {
                    step.hold_ms = *calibrate_step_ms;
                }
            }
            if (calibrate_cooldown_ms.has_value() &&
                !options.sequence.empty()) {
                options.sequence.back().hold_ms = *calibrate_cooldown_ms;
            }
            if (calibrate_settle_window_ms.has_value()) {
                options.settle_window_ms = *calibrate_settle_window_ms;
            }
            if (calibrate_abort_temp_c.has_value()) {
                options.abort_temp_ceiling_c = *calibrate_abort_temp_c;
            }
            if (calibrate_channel.has_value()) {
                options.only_channel = *calibrate_channel;
            }
            if (!calibrate_output_path.empty()) {
                options.output_path = calibrate_output_path;
            }
            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const svg_mb_control::ControlConfig effective_config =
                config.has_value() ? *config
                                   : svg_mb_control::ControlConfig{};
            const int result = svg_mb_control::RunCalibration(
                effective_config, reconcile_runtime_home, options,
                g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kReadLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode read-loop requires a control config.");
            }

            const std::filesystem::path runtime_home =
                svg_mb_control::ResolveRuntimeHomePath(*config);
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintReadLoopStartup(*config, runtime_home, runtime_policy);

            svg_mb_control::ReadLoop loop(*config, runtime_home);
            g_active_read_loop = &loop;
            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                g_active_read_loop = nullptr;
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }

            const int loop_exit = loop.RunUntilStopped();

            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            g_active_read_loop = nullptr;
            return loop_exit;
        }

        if (run_mode == RunMode::kEvidenceLog) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode evidence-log requires a control config.");
            }

            const std::filesystem::path runtime_home =
                svg_mb_control::ResolveRuntimeHomePath(*config);
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintEvidenceLogStartup(*config, runtime_home, runtime_policy);

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }

            const int result = svg_mb_control::RunEvidenceLog(
                *config, runtime_home, g_stop_signaled);

            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        const std::string snapshot_json = SampleDirectSnapshotJson(
            config.has_value() ? &*config : nullptr);
        std::cout << snapshot_json;
        if (snapshot_json.empty() || snapshot_json.back() != '\n') {
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
