#include "startup_banner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace svg_mb_control {
namespace {

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
                            const ControlConfig& config,
                            const std::filesystem::path& runtime_home,
                            const RuntimeWritePolicy& policy) {
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

}  // namespace

void PrintControlLoopStartup(const ControlConfig& config,
                             const ControlLoopConfig& loop_config,
                             const std::filesystem::path& runtime_home,
                             const RuntimeWritePolicy& policy) {
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
                  << TempBlendToString(channel.temp_blend)
                  << " shape="
                  << CurveShapeToString(channel.curve_shape)
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

void PrintReadLoopStartup(const ControlConfig& config,
                          const std::filesystem::path& runtime_home,
                          const RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("read-loop", config, runtime_home, policy);
    std::cout << "  poll_ms: " << config.poll_ms << '\n' << std::flush;
}

void PrintEvidenceLogStartup(const ControlConfig& config,
                             const std::filesystem::path& runtime_home,
                             const RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("evidence-log", config, runtime_home, policy);
    std::cout << "  poll_ms: " << config.poll_ms << '\n'
              << "  evidence_gpu_sample_mode: "
              << config.evidence_gpu_sample_mode << '\n'
              << std::flush;
}

}  // namespace svg_mb_control
