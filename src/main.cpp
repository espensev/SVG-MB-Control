#include "bench_bridge.h"
#include "control_config.h"
#include "read_loop.h"
#include "write_orchestrator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <cwctype>

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

enum class BridgeCommand {
    kLoggerService,
    kReadSnapshot,
};

enum class RunMode {
    kOneShot,
    kReadLoop,
    kWriteOnce,
};

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
        << "  svg-mb-control [--mode <one-shot|read-loop>] [--config <path>] "
           << "[--bench-exe-path <path>] "
           << "[--bridge-command <logger-service|read-snapshot>] "
           << "[--duration-ms <ms>] [--timeout-ms <ms>]\n"
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

BridgeCommand ParseBridgeCommand(const wchar_t* value) {
    const std::wstring raw(value);
    if (raw == L"logger-service") {
        return BridgeCommand::kLoggerService;
    }
    if (raw == L"read-snapshot") {
        return BridgeCommand::kReadSnapshot;
    }
    throw std::runtime_error("Invalid --bridge-command value.");
}

RunMode ParseRunMode(const wchar_t* value) {
    const std::wstring raw(value);
    if (raw == L"one-shot") {
        return RunMode::kOneShot;
    }
    if (raw == L"read-loop") {
        return RunMode::kReadLoop;
    }
    if (raw == L"write-once") {
        return RunMode::kWriteOnce;
    }
    throw std::runtime_error("Invalid --mode value.");
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

std::uint32_t ParseTimeoutMs(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        if (parsed == 0ul) {
            throw std::runtime_error("timeout must be greater than zero");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --timeout-ms value.");
    }
}

std::uint32_t ParseDurationMs(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        if (parsed == 0ul) {
            throw std::runtime_error("duration must be greater than zero");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --duration-ms value.");
    }
}

bool PathsMatch(const std::filesystem::path& lhs,
                const std::filesystem::path& rhs) {
    std::error_code ec;
    if (std::filesystem::exists(lhs, ec) && std::filesystem::exists(rhs, ec)) {
        if (std::filesystem::equivalent(lhs, rhs, ec) && !ec) {
            return true;
        }
    }

    auto normalize = [](const std::filesystem::path& value) {
        std::wstring text = value.wstring();
        for (wchar_t& ch : text) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        return text;
    };

    return normalize(lhs) == normalize(rhs);
}

std::filesystem::path ResolveJsonArtifactPath(
    BridgeCommand command,
    const svg_mb_control::JsonArtifactLaunchResult& launch,
    const std::optional<svg_mb_control::ControlConfig>& config) {
    if (command == BridgeCommand::kReadSnapshot) {
        return launch.json_artifact_path;
    }

    if (config.has_value() && !config->snapshot_path.empty()) {
        if (!launch.json_artifact_path.empty() &&
            !PathsMatch(config->snapshot_path, launch.json_artifact_path)) {
            throw std::runtime_error(
                "Bench logger-service snapshot_path does not match control config snapshot_path.");
        }
        return config->snapshot_path;
    }

    return launch.json_artifact_path;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path config_path;
        bool config_path_explicit = false;
        std::filesystem::path bench_exe_path;
        BridgeCommand bridge_command = BridgeCommand::kLoggerService;
        RunMode run_mode = RunMode::kOneShot;
        std::uint32_t duration_ms = 0u;
        bool duration_ms_explicit = false;
        std::uint32_t timeout_ms = 15000u;
        std::uint32_t write_channel = 0u;
        bool write_channel_explicit = false;
        double write_pct = 0.0;
        bool write_pct_explicit = false;
        std::uint32_t write_hold_ms = 0u;
        bool write_hold_ms_explicit = false;

        for (int index = 1; index < argc; ++index) {
            const std::wstring arg = argv[index];
            auto require_value = [&]() -> const wchar_t* {
                if (index + 1 >= argc) {
                    throw std::runtime_error("Missing value for option.");
                }
                ++index;
                return argv[index];
            };

            if (arg == L"--bench-exe-path") {
                bench_exe_path = std::filesystem::path(require_value());
            } else if (arg == L"--config") {
                config_path = std::filesystem::path(require_value());
                config_path_explicit = true;
            } else if (arg == L"--bridge-command") {
                bridge_command = ParseBridgeCommand(require_value());
            } else if (arg == L"--mode") {
                run_mode = ParseRunMode(require_value());
            } else if (arg == L"--duration-ms") {
                duration_ms = ParseDurationMs(require_value());
                duration_ms_explicit = true;
            } else if (arg == L"--timeout-ms") {
                timeout_ms = ParseTimeoutMs(require_value());
            } else if (arg == L"--write-channel") {
                write_channel = ParseWriteChannel(require_value());
                write_channel_explicit = true;
            } else if (arg == L"--write-pct") {
                write_pct = ParseWritePct(require_value());
                write_pct_explicit = true;
            } else if (arg == L"--write-hold-ms") {
                write_hold_ms = ParseWriteHoldMs(require_value());
                write_hold_ms_explicit = true;
            } else if (arg == L"--help" || arg == L"-h") {
                PrintUsage();
                return 0;
            } else if (arg == L"--version") {
                PrintVersion();
                return 0;
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

        if (bench_exe_path.empty()) {
            bench_exe_path = svg_mb_control::GetEnvironmentPath(L"SVG_MB_CONTROL_BENCH_EXE");
        }
        if (bench_exe_path.empty() && config.has_value()) {
            bench_exe_path = config->bench_exe_path;
        }
        if (bench_exe_path.empty()) {
            bench_exe_path = svg_mb_control::ResolveDefaultBenchExecutablePath();
        }
        if (bench_exe_path.empty()) {
            throw std::runtime_error("Could not resolve svg-mb-bench.exe. Pass --bench-exe-path or configure bench_exe_path.");
        }

        // Unconditional startup reconciliation of any pending writes from a
        // prior crashed Control session, before dispatching on run mode.
        const std::uint32_t reconcile_timeout_ms =
            config.has_value() ? config->restore_timeout_ms : 5000u;
        const std::filesystem::path reconcile_runtime_home =
            config.has_value()
                ? svg_mb_control::ResolveRuntimeHomePath(*config)
                : svg_mb_control::ResolveRuntimeHomePath(
                      svg_mb_control::ControlConfig{});
        const int reconcile_result = svg_mb_control::ReconcilePendingWrites(
            bench_exe_path.wstring(), reconcile_runtime_home,
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
                *config, bench_exe_path.wstring(),
                reconcile_runtime_home, request, g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kReadLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode read-loop requires a control config with snapshot_path.");
            }
            if (config->snapshot_path.empty()) {
                throw std::runtime_error("--mode read-loop requires snapshot_path in the control config.");
            }
            if (duration_ms_explicit && duration_ms > 0u) {
                config->logger_service_duration_ms = duration_ms;
            }

            const std::filesystem::path runtime_home =
                svg_mb_control::ResolveRuntimeHomePath(*config);

            svg_mb_control::ReadLoop loop(*config, runtime_home,
                                          bench_exe_path.wstring());
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

        if (!duration_ms_explicit && config.has_value()) {
            duration_ms = config->poll_ms;
        }
        if (duration_ms == 0u) {
            duration_ms = 1000u;
        }
        if (bridge_command == BridgeCommand::kReadSnapshot && duration_ms_explicit) {
            throw std::runtime_error("--duration-ms is only valid with --bridge-command logger-service.");
        }

        svg_mb_control::JsonArtifactLaunchResult launch;
        if (bridge_command == BridgeCommand::kLoggerService) {
            launch = svg_mb_control::RunLoggerService(
                bench_exe_path.wstring(), duration_ms, timeout_ms);
        } else {
            launch = svg_mb_control::RunReadSnapshot(
                bench_exe_path.wstring(), timeout_ms);
        }

        const std::filesystem::path json_artifact_path = ResolveJsonArtifactPath(
            bridge_command, launch, config);
        const std::string snapshot_json =
            svg_mb_control::LoadJsonObjectFile(json_artifact_path);

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
