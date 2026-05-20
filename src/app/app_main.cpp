#include "app/app_main.h"

#include "amd_reader.h"
#include "analyze/analyze_cli.h"
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
#include "startup_banner.h"
#include "write_orchestrator.h"

#include "windows_lean.h"

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

std::atomic<svg_mb_control::ReadLoop*> g_active_read_loop{nullptr};
std::atomic<svg_mb_control::ControlLoop*> g_active_control_loop{nullptr};
std::atomic<bool> g_stop_signaled{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop_signaled.store(true);
            if (auto* read_loop = g_active_read_loop.load()) {
                read_loop->RequestStop();
            }
            if (auto* control_loop = g_active_control_loop.load()) {
                control_loop->RequestStop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

class ConsoleCtrlScope {
public:
    ConsoleCtrlScope() = default;
    ConsoleCtrlScope(const ConsoleCtrlScope&) = delete;
    ConsoleCtrlScope& operator=(const ConsoleCtrlScope&) = delete;

    ~ConsoleCtrlScope() {
        Reset();
    }

    void Install() {
        if (installed_) {
            return;
        }
        if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
            throw std::runtime_error("SetConsoleCtrlHandler failed.");
        }
        installed_ = true;
    }

    void Reset() noexcept {
        if (installed_) {
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            installed_ = false;
        }
    }

private:
    bool installed_ = false;
};

class ActiveControlLoopScope {
public:
    explicit ActiveControlLoopScope(svg_mb_control::ControlLoop& control_loop) {
        g_active_control_loop.store(&control_loop);
        try {
            console_ctrl_.Install();
        } catch (...) {
            g_active_control_loop.store(nullptr);
            throw;
        }
    }

    ActiveControlLoopScope(const ActiveControlLoopScope&) = delete;
    ActiveControlLoopScope& operator=(const ActiveControlLoopScope&) = delete;

    ~ActiveControlLoopScope() {
        console_ctrl_.Reset();
        g_active_control_loop.store(nullptr);
    }

private:
    ConsoleCtrlScope console_ctrl_;
};

class ActiveReadLoopScope {
public:
    explicit ActiveReadLoopScope(svg_mb_control::ReadLoop& read_loop) {
        g_active_read_loop.store(&read_loop);
        try {
            console_ctrl_.Install();
        } catch (...) {
            g_active_read_loop.store(nullptr);
            throw;
        }
    }

    ActiveReadLoopScope(const ActiveReadLoopScope&) = delete;
    ActiveReadLoopScope& operator=(const ActiveReadLoopScope&) = delete;

    ~ActiveReadLoopScope() {
        console_ctrl_.Reset();
        g_active_read_loop.store(nullptr);
    }

private:
    ConsoleCtrlScope console_ctrl_;
};

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
        << "  svg-mb-control analyze report [--runtime-home <path>] "
           << "[--db <path>] [--run <id>|--session <ts>] [--idle-seconds <s>] "
           << "[--load-threshold-c <c>] [--json]\n"
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

int svg_mb_control::RunApp(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"analyze") {
            return svg_mb_control::analyze::RunAnalyzeCommand(argc, argv);
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
                calibrate_sequence =
                    svg_mb_control::ParseCalibrationSequence(require_value());
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

            ConsoleCtrlScope console_ctrl;
            console_ctrl.Install();
            const int result = svg_mb_control::RunWriteOnce(
                *config, reconcile_runtime_home, request, g_stop_signaled);
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

            ActiveControlLoopScope active_loop(control_loop);
            const int result = control_loop.RunUntilStopped(g_stop_signaled);
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
            ConsoleCtrlScope console_ctrl;
            console_ctrl.Install();
            const svg_mb_control::ControlConfig effective_config =
                config.has_value() ? *config
                                   : svg_mb_control::ControlConfig{};
            const int result = svg_mb_control::RunCalibration(
                effective_config, reconcile_runtime_home, options,
                g_stop_signaled);
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
            ActiveReadLoopScope active_loop(loop);

            const int loop_exit = loop.RunUntilStopped();

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

            ConsoleCtrlScope console_ctrl;
            console_ctrl.Install();

            const int result = svg_mb_control::RunEvidenceLog(
                *config, runtime_home, g_stop_signaled);

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
