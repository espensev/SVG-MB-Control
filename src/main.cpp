#include "amd_reader.h"
#include "control_config.h"
#include "control_loop.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "read_loop.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"
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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

enum class RunMode {
    kOneShot,
    kReadLoop,
    kWriteOnce,
    kControlLoop,
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
        << "  svg-mb-control [--mode <one-shot|read-loop|write-once|control-loop>] [--config <path>] "
           << "[--write-channel <n>] [--write-pct <pct>] [--write-hold-ms <ms>]\n"
        << "  svg-mb-control --diagnose-amd\n"
        << "  svg-mb-control --diagnose-gpu\n"
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
    if (raw == L"control-loop") {
        return RunMode::kControlLoop;
    }
    throw std::runtime_error("Invalid --mode value.");
}

RunMode ParseRunMode(std::string_view value) {
    if (value == "one-shot") {
        return RunMode::kOneShot;
    }
    if (value == "read-loop") {
        return RunMode::kReadLoop;
    }
    if (value == "write-once") {
        return RunMode::kWriteOnce;
    }
    if (value == "control-loop") {
        return RunMode::kControlLoop;
    }
    throw std::runtime_error("Invalid default_mode in control config.");
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
        std::filesystem::path config_path;
        bool config_path_explicit = false;
        RunMode run_mode = RunMode::kOneShot;
        bool run_mode_explicit = false;
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

            if (arg == L"--config") {
                config_path = std::filesystem::path(require_value());
                config_path_explicit = true;
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

            svg_mb_control::ControlLoop control_loop(
                *config, loop_config, reconcile_runtime_home);

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const int result = control_loop.RunUntilStopped(g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kReadLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode read-loop requires a control config.");
            }

            const std::filesystem::path runtime_home =
                svg_mb_control::ResolveRuntimeHomePath(*config);

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
