#include "app/app_main.h"

#include "analyze/analyze_cli.h"
#include "app/app_args.h"
#include "app/app_diagnose.h"
#include "app/app_signals.h"
#include "calibration.h"
#include "control_config.h"
#include "control_config_print.h"
#include "control_loop.h"
#include "control_supervisor.h"
#include "evidence_log.h"
#include "read_loop.h"
#include "runtime_health.h"
#include "runtime_lifecycle.h"
#include "runtime_singleton.h"
#include "runtime_write_policy.h"
#include "service_probe.h"
#include "startup_banner.h"
#include "worker_force_terminate.h"
#include "write_orchestrator.h"

#include "windows_lean.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using svg_mb_control::ConfirmDetachedLaunch;
using svg_mb_control::IsLongRunningMode;
using svg_mb_control::LaunchDetachedLongRunningMode;
using svg_mb_control::ParseRunMode;
using svg_mb_control::PrintRuntimeHealth;
using svg_mb_control::PrintRuntimeStatus;
using svg_mb_control::RequestStopAndWait;
using svg_mb_control::RunMode;
using svg_mb_control::RunSupervisedLongRunningMode;

}  // namespace

int svg_mb_control::RunApp(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"analyze") {
            return svg_mb_control::analyze::RunAnalyzeCommand(argc, argv);
        }
        const bool no_launch_args = argc == 1;
        CliOptions options = ParseCliOptions(argc, argv);

        if (options.help_requested) {
            PrintUsage();
            return 0;
        }
        if (options.version_requested) {
            PrintVersion();
            return 0;
        }
        if (options.diagnose_amd_requested) {
            return RunDiagnoseAmd();
        }
        if (options.diagnose_gpu_requested) {
            return RunDiagnoseGpu();
        }

        if (options.config_path.empty()) {
            options.config_path =
                svg_mb_control::GetEnvironmentPath(L"SVG_MB_CONTROL_CONFIG");
            if (!options.config_path.empty()) {
                options.config_path_explicit = true;
            }
        }
        if (options.config_path.empty()) {
            options.config_path =
                svg_mb_control::ResolveDefaultControlConfigPath();
        }

        std::optional<svg_mb_control::ControlConfig> config;
        if (!options.config_path.empty()) {
            const std::filesystem::path absolute_config_path =
                std::filesystem::absolute(options.config_path)
                    .lexically_normal();
            if (!std::filesystem::exists(absolute_config_path)) {
                if (options.config_path_explicit) {
                    throw std::runtime_error("Control config not found: " +
                                             absolute_config_path.string());
                }
            } else {
                config = svg_mb_control::LoadControlConfig(absolute_config_path);
            }
        }

        if (!options.run_mode_explicit && config.has_value() &&
            !config->default_mode.empty()) {
            options.run_mode = ParseRunMode(config->default_mode);
        }

        const svg_mb_control::ControlConfig status_config =
            config.has_value() ? *config : svg_mb_control::ControlConfig{};
        const std::filesystem::path command_runtime_home =
            svg_mb_control::ResolveRuntimeHomePath(status_config);
        const std::uint32_t health_stale_after_ms =
            status_config.staleness_threshold_ms > 0u
                ? status_config.staleness_threshold_ms
                : 10000u;

        if (options.json_output_requested && !options.status_requested &&
            !options.health_requested && !options.service_probe_requested &&
            !options.show_config_requested) {
            throw std::runtime_error(
                "--json requires --status, --health, --service-probe, or --show-config.");
        }

        if (options.reset_breakers_requested &&
            (options.start_requested || options.status_requested ||
             options.health_requested || options.service_probe_requested ||
             options.stop_requested || options.restart_requested ||
             options.foreground_launch || options.supervisor_launch)) {
            throw std::runtime_error(
                "--reset-breakers cannot be combined with another runtime command.");
        }

        if (options.show_config_requested) {
            if (!config.has_value()) {
                throw std::runtime_error(
                    "--show-config requires a control config (set --config or "
                    "SVG_MB_CONTROL_CONFIG).");
            }
            std::optional<svg_mb_control::ControlLoopConfig> loop_config;
            try {
                loop_config = svg_mb_control::LoadControlLoopConfig(
                    config->source_path);
            } catch (const std::exception&) {
                // Config has no control_loop subtree (e.g. read-loop/write-once
                // only). Fall through and print only the base summary.
            }
            svg_mb_control::PrintControlConfigSummary(
                std::cout, *config, loop_config,
                options.json_output_requested);
            return 0;
        }

        if (options.service_probe_requested) {
            return svg_mb_control::RunServiceProbe(
                command_runtime_home,
                config.has_value() ? &*config : nullptr,
                options.json_output_requested);
        }

        if (options.health_requested ||
            (options.status_requested && options.json_output_requested)) {
            return PrintRuntimeHealth(command_runtime_home,
                                      options.json_output_requested,
                                      health_stale_after_ms);
        }

        if (options.status_requested) {
            return PrintRuntimeStatus(command_runtime_home);
        }

        if (options.reset_breakers_requested) {
            if (!svg_mb_control::RequestRuntimeBreakerReset(
                    command_runtime_home, options.reset_breaker_channel)) {
                std::cerr
                    << "Error: failed to write circuit-breaker reset request.\n";
                return 1;
            }
            std::cout << "svg-mb-control: circuit-breaker reset requested\n"
                      << "  channel: ";
            if (options.reset_breaker_channel.has_value()) {
                std::cout << *options.reset_breaker_channel;
            } else {
                std::cout << "all";
            }
            std::cout << '\n'
                      << "  request: "
                      << svg_mb_control::RuntimeBreakerResetRequestPath(
                             command_runtime_home)
                             .string()
                      << '\n';
            return 0;
        }

        if (options.stop_requested && !options.restart_requested) {
            return RequestStopAndWait(command_runtime_home);
        }

        if (options.restart_requested) {
            const int stop_result =
                RequestStopAndWait(command_runtime_home, true);
            if (stop_result != 0) {
                // stop_result == 2 is the hung-worker case (the graceful stop
                // timed out). Escalate to a bounded force-terminate
                // (FEAT-0008 / REQ-WATCHDOG-01): if the worker -- and the
                // supervisor, if it does not self-exit -- are confirmed gone,
                // relaunch proceeds; otherwise return the original error and let
                // the watchdog retry on its next poll. stop_result == 1 (could
                // not even write the stop request) is not a hung worker, so it
                // stays on the unchanged return-without-relaunch path.
                if (stop_result != 2 ||
                    !svg_mb_control::EscalateForceTerminate(
                        command_runtime_home, stop_result)) {
                    return stop_result;
                }
            }
            options.start_requested = true;
        }

        if (options.supervisor_launch) {
            if (!config.has_value()) {
                throw std::runtime_error(
                    "--run-supervisor requires a control config.");
            }
            if (!IsLongRunningMode(options.run_mode)) {
                throw std::runtime_error(
                    "--run-supervisor requires read-loop or control-loop.");
            }
            return RunSupervisedLongRunningMode(options.run_mode, *config);
        }

        if (!options.foreground_launch && config.has_value() &&
            IsLongRunningMode(options.run_mode) &&
            (no_launch_args || options.confirm_start ||
             options.start_requested)) {
            if (options.confirm_start &&
                !ConfirmDetachedLaunch(options.run_mode, *config)) {
                std::cout << "svg-mb-control: start cancelled\n";
                return 0;
            }
            return LaunchDetachedLongRunningMode(options.run_mode, *config);
        }

        if (options.start_requested || options.restart_requested) {
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

        std::optional<svg_mb_control::SingletonAcquisition> worker_singleton;
        auto acquire_worker_singleton = [&]() -> int {
            worker_singleton = svg_mb_control::TryAcquireRuntimeSingleton(
                svg_mb_control::SingletonRole::kWorker,
                reconcile_runtime_home);
            if (worker_singleton->acquired) {
                return 0;
            }
            std::cerr << "Error: another svg-mb-control worker is "
                      << "already running for runtime_home="
                      << reconcile_runtime_home.string() << '\n';
            if (!worker_singleton->diagnostic.empty()) {
                std::cerr << "  detail: " << worker_singleton->diagnostic
                          << '\n';
            }
            return 2;
        };

        if (options.run_mode == RunMode::kControlLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode control-loop requires a control config.");
            }
            if (options.config_path.empty()) {
                throw std::runtime_error("--mode control-loop requires a resolvable config path.");
            }
            const int singleton_result = acquire_worker_singleton();
            if (singleton_result != 0) {
                return singleton_result;
            }
        } else if (options.run_mode == RunMode::kReadLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode read-loop requires a control config.");
            }
            const int singleton_result = acquire_worker_singleton();
            if (singleton_result != 0) {
                return singleton_result;
            }
        }

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

        if (options.run_mode == RunMode::kWriteOnce) {
            if (!config.has_value()) {
                svg_mb_control::ControlConfig defaults;
                config = defaults;
            }
            if (!options.write_channel_explicit &&
                !config->write_channel_set) {
                throw std::runtime_error("--mode write-once requires --write-channel or write_channel in config.");
            }
            if (!options.write_pct_explicit &&
                !config->write_target_pct_set) {
                throw std::runtime_error("--mode write-once requires --write-pct or write_target_pct in config.");
            }
            if (!options.write_hold_ms_explicit &&
                !config->write_hold_ms_set) {
                throw std::runtime_error("--mode write-once requires --write-hold-ms or write_hold_ms in config.");
            }
            svg_mb_control::WriteRequest request;
            request.channel = options.write_channel_explicit
                ? options.write_channel : config->write_channel;
            request.target_pct = options.write_pct_explicit
                ? options.write_pct : config->write_target_pct;
            request.hold_ms = options.write_hold_ms_explicit
                ? options.write_hold_ms : config->write_hold_ms;

            ConsoleCtrlScope console_ctrl;
            console_ctrl.Install();
            return svg_mb_control::RunWriteOnce(
                *config, reconcile_runtime_home, request, StopSignaled());
        }

        if (options.run_mode == RunMode::kControlLoop) {
            const svg_mb_control::ControlLoopConfig loop_config =
                svg_mb_control::LoadControlLoopConfig(
                    std::filesystem::absolute(options.config_path)
                        .lexically_normal());
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintControlLoopStartup(
                *config, loop_config, reconcile_runtime_home, runtime_policy);

            svg_mb_control::ControlLoop control_loop(
                *config, loop_config, reconcile_runtime_home);

            ActiveControlLoopScope active_loop(control_loop);
            return control_loop.RunUntilStopped(StopSignaled());
        }

        if (options.run_mode == RunMode::kCalibrate) {
            if (options.calibrate_sequence.has_value() &&
                (options.calibrate_step_ms.has_value() ||
                 options.calibrate_cooldown_ms.has_value())) {
                throw std::runtime_error(
                    "--calibrate-sequence cannot be combined with "
                    "--calibrate-step-ms or --calibrate-cooldown-ms.");
            }
            svg_mb_control::CalibrationOptions calibrate_options =
                svg_mb_control::DefaultCalibrationOptions();
            if (options.calibrate_sequence.has_value()) {
                calibrate_options.sequence = *options.calibrate_sequence;
            }
            if (options.calibrate_step_ms.has_value()) {
                for (auto& step : calibrate_options.sequence) {
                    step.hold_ms = *options.calibrate_step_ms;
                }
            }
            if (options.calibrate_cooldown_ms.has_value() &&
                !calibrate_options.sequence.empty()) {
                calibrate_options.sequence.back().hold_ms =
                    *options.calibrate_cooldown_ms;
            }
            if (options.calibrate_settle_window_ms.has_value()) {
                calibrate_options.settle_window_ms =
                    *options.calibrate_settle_window_ms;
            }
            if (options.calibrate_abort_temp_c.has_value()) {
                calibrate_options.abort_temp_ceiling_c =
                    *options.calibrate_abort_temp_c;
            }
            if (options.calibrate_channel.has_value()) {
                calibrate_options.only_channel = *options.calibrate_channel;
            }
            if (!options.calibrate_output_path.empty()) {
                calibrate_options.output_path = options.calibrate_output_path;
            }
            ConsoleCtrlScope console_ctrl;
            console_ctrl.Install();
            const svg_mb_control::ControlConfig effective_config =
                config.has_value() ? *config
                                   : svg_mb_control::ControlConfig{};
            return svg_mb_control::RunCalibration(
                effective_config, reconcile_runtime_home, calibrate_options,
                StopSignaled());
        }

        if (options.run_mode == RunMode::kReadLoop) {
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintReadLoopStartup(*config, reconcile_runtime_home, runtime_policy);

            svg_mb_control::ReadLoop loop(*config, reconcile_runtime_home);
            ActiveReadLoopScope active_loop(loop);
            return loop.RunUntilStopped();
        }

        if (options.run_mode == RunMode::kEvidenceLog) {
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
            return svg_mb_control::RunEvidenceLog(
                *config, runtime_home, StopSignaled());
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
