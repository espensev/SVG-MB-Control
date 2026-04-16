#include "control_loop.h"

#include "amd_reader.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace svg_mb_control {

namespace {

// ---------- Minimal JSON scanning helpers (local to this TU) ----------

std::size_t SkipWs(const std::string& t, std::size_t o, std::size_t limit) {
    while (o < limit &&
           std::isspace(static_cast<unsigned char>(t[o])) != 0) {
        ++o;
    }
    return o;
}

bool FindObjectRange(const std::string& text, std::string_view key,
                     std::size_t range_begin, std::size_t range_end,
                     std::size_t* out_begin, std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return false;
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '{') return false;
    std::size_t depth = 1u;
    std::size_t cursor = val_start + 1u;
    bool in_str = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_str) {
            if (ch == '\\' && cursor + 1u < range_end) { cursor += 2u; continue; }
            if (ch == '"') in_str = false;
            ++cursor;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '{') ++depth;
        else if (ch == '}') { --depth; if (depth == 0u) break; }
        ++cursor;
    }
    if (depth != 0u) return false;
    *out_begin = val_start + 1u;
    *out_end = cursor;
    return true;
}

bool FindArrayRange(const std::string& text, std::string_view key,
                    std::size_t range_begin, std::size_t range_end,
                    std::size_t* out_begin, std::size_t* out_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return false;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return false;
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '[') return false;
    std::size_t depth = 1u;
    std::size_t cursor = val_start + 1u;
    bool in_str = false;
    while (cursor < range_end && depth > 0u) {
        const char ch = text[cursor];
        if (in_str) {
            if (ch == '\\' && cursor + 1u < range_end) { cursor += 2u; continue; }
            if (ch == '"') in_str = false;
            ++cursor;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '[') ++depth;
        else if (ch == ']') { --depth; if (depth == 0u) break; }
        ++cursor;
    }
    if (depth != 0u) return false;
    *out_begin = val_start + 1u;
    *out_end = cursor;
    return true;
}

template <class F>
void ForEachObjectInRange(const std::string& text,
                          std::size_t range_begin, std::size_t range_end,
                          F&& visit) {
    std::size_t cursor = range_begin;
    while (cursor < range_end) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= range_end) break;
        std::size_t depth = 1u;
        std::size_t obj_close = obj_open + 1u;
        bool in_str = false;
        while (obj_close < range_end && depth > 0u) {
            const char ch = text[obj_close];
            if (in_str) {
                if (ch == '\\' && obj_close + 1u < range_end) { obj_close += 2u; continue; }
                if (ch == '"') in_str = false;
                ++obj_close;
                continue;
            }
            if (ch == '"') in_str = true;
            else if (ch == '{') ++depth;
            else if (ch == '}') { --depth; if (depth == 0u) break; }
            ++obj_close;
        }
        if (depth != 0u) break;
        visit(obj_open, obj_close);
        cursor = obj_close + 1u;
    }
}

double FindNumericInRange(const std::string& text, std::string_view key,
                          std::size_t range_begin, std::size_t range_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    std::size_t val_end = val_start;
    while (val_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[val_end])) != 0 ||
            text[val_end] == '.' || text[val_end] == '-' ||
            text[val_end] == '+' || text[val_end] == 'e' ||
            text[val_end] == 'E')) {
        ++val_end;
    }
    if (val_end == val_start) return std::numeric_limits<double>::quiet_NaN();
    try {
        return std::stod(text.substr(val_start, val_end - val_start));
    } catch (const std::exception&) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::string FindStringInRange(const std::string& text, std::string_view key,
                              std::size_t range_begin, std::size_t range_end) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) return {};
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) return {};
    const std::size_t val_start = SkipWs(text, colon + 1u, range_end);
    if (val_start >= range_end || text[val_start] != '"') return {};
    std::string output;
    for (std::size_t i = val_start + 1u; i < range_end; ++i) {
        const char ch = text[i];
        if (ch == '\\') {
            if (i + 1u >= range_end) return {};
            const char e = text[++i];
            switch (e) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return {};
            }
            continue;
        }
        if (ch == '"') return output;
        output.push_back(ch);
    }
    return {};
}

std::string ReadEntireFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open control config: " +
                                 path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) return {};
    std::array<char, 32> buf{};
    const std::size_t n = std::strftime(buf.data(), buf.size(),
                                        "%Y-%m-%dT%H:%M:%S", &local);
    return n > 0 ? std::string(buf.data(), n) : std::string();
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2u);
    for (char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::array<char, 8> esc{};
                    std::snprintf(esc.data(), esc.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += esc.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

std::uint32_t EffectiveWriteCooldownMs(const ControlLoopConfig& loop,
                                       const ChannelControlConfig& channel) {
    return channel.write_cooldown_ms > 0u
        ? channel.write_cooldown_ms
        : loop.write_cooldown_ms;
}

double EffectiveDeadbandPct(const ControlLoopConfig& loop,
                            const ChannelControlConfig& channel) {
    return std::isnan(channel.deadband_pct)
        ? loop.deadband_pct
        : channel.deadband_pct;
}

std::uint32_t EffectiveControlHoldMs(const ControlLoopConfig& loop,
                                     const ChannelControlConfig& channel) {
    return channel.control_hold_ms > 0u
        ? channel.control_hold_ms
        : loop.control_hold_ms;
}

}  // namespace

// ------------------------ Config loader --------------------------------

ControlLoopConfig LoadControlLoopConfig(
    const std::filesystem::path& config_path) {
    const std::string text = ReadEntireFile(config_path);

    std::size_t loop_begin = 0u;
    std::size_t loop_end = 0u;
    if (!FindObjectRange(text, "control_loop", 0u, text.size(),
                         &loop_begin, &loop_end)) {
        throw std::runtime_error(
            "control config missing control_loop object: " + config_path.string());
    }

    ControlLoopConfig cfg;

    const double poll_tick = FindNumericInRange(text, "poll_tick_ms", loop_begin, loop_end);
    if (!std::isnan(poll_tick) && poll_tick > 0.0) {
        cfg.poll_tick_ms = static_cast<std::uint32_t>(poll_tick);
    }
    const double cooldown = FindNumericInRange(text, "write_cooldown_ms", loop_begin, loop_end);
    if (!std::isnan(cooldown)) {
        cfg.write_cooldown_ms = static_cast<std::uint32_t>(cooldown);
    }
    const double deadband = FindNumericInRange(text, "deadband_pct", loop_begin, loop_end);
    if (!std::isnan(deadband)) {
        cfg.deadband_pct = deadband;
    }
    const double hold = FindNumericInRange(text, "control_hold_ms", loop_begin, loop_end);
    if (!std::isnan(hold) && hold > 0.0) {
        cfg.control_hold_ms = static_cast<std::uint32_t>(hold);
    }
    const std::string cpu_label = FindStringInRange(text, "cpu_temp_label", loop_begin, loop_end);
    if (!cpu_label.empty()) {
        cfg.cpu_temp_label = cpu_label;
    }

    std::size_t ch_begin = 0u;
    std::size_t ch_end = 0u;
    if (!FindArrayRange(text, "channels", loop_begin, loop_end, &ch_begin, &ch_end)) {
        throw std::runtime_error(
            "control_loop config missing channels array: " + config_path.string());
    }

    ForEachObjectInRange(text, ch_begin, ch_end,
        [&](std::size_t obj_open, std::size_t obj_close) {
            ChannelControlConfig channel;
            const double ch_idx = FindNumericInRange(text, "channel", obj_open, obj_close);
            if (std::isnan(ch_idx)) return;
            channel.channel = static_cast<std::uint32_t>(ch_idx);
            const double floor = FindNumericInRange(text, "min_duty_pct", obj_open, obj_close);
            if (!std::isnan(floor)) channel.min_duty_pct = floor;
            const double cooldown = FindNumericInRange(text, "write_cooldown_ms", obj_open, obj_close);
            if (!std::isnan(cooldown) && cooldown > 0.0) {
                channel.write_cooldown_ms = static_cast<std::uint32_t>(cooldown);
            }
            const double deadband = FindNumericInRange(text, "deadband_pct", obj_open, obj_close);
            if (!std::isnan(deadband)) {
                channel.deadband_pct = deadband;
            }
            const double hold = FindNumericInRange(text, "control_hold_ms", obj_open, obj_close);
            if (!std::isnan(hold) && hold > 0.0) {
                channel.control_hold_ms = static_cast<std::uint32_t>(hold);
            }
            const std::string blend = FindStringInRange(text, "temp_blend", obj_open, obj_close);
            if (!blend.empty()) {
                try {
                    channel.temp_blend = ParseTempBlend(blend);
                } catch (const std::exception&) {
                    channel.temp_blend = TempBlend::CpuOnly;
                }
            }
            std::size_t curve_begin = 0u;
            std::size_t curve_end = 0u;
            if (FindArrayRange(text, "curve", obj_open, obj_close,
                               &curve_begin, &curve_end)) {
                ForEachObjectInRange(text, curve_begin, curve_end,
                    [&](std::size_t p_open, std::size_t p_close) {
                        const double t = FindNumericInRange(text, "temp_c", p_open, p_close);
                        const double d = FindNumericInRange(text, "duty_pct", p_open, p_close);
                        if (std::isnan(t) || std::isnan(d)) return;
                        channel.curve.push_back({t, d});
                    });
                std::sort(channel.curve.begin(), channel.curve.end(),
                          [](const CurvePoint& a, const CurvePoint& b) {
                              return a.temp_c < b.temp_c;
                          });
            }
            cfg.channels.push_back(std::move(channel));
        });

    if (cfg.channels.empty()) {
        throw std::runtime_error(
            "control_loop config has empty channels array: " + config_path.string());
    }
    return cfg;
}

// ------------------------ ControlLoop Impl -----------------------------

struct ChannelState {
    ChannelControlConfig config;
    bool baseline_captured = false;
    std::uint8_t baseline_duty_raw = 0u;
    std::uint8_t baseline_mode_raw = 0u;
    double last_issued_pct = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point last_write_time =
        std::chrono::steady_clock::time_point{};
    bool write_active = false;
    std::chrono::steady_clock::time_point hold_deadline =
        std::chrono::steady_clock::time_point{};
    std::uint64_t total_writes = 0u;
    double last_observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double last_setpoint_pct = std::numeric_limits<double>::quiet_NaN();
};

struct ControlLoop::Impl {
    ControlConfig base;
    ControlLoopConfig loop;
    std::filesystem::path runtime_home;
    RuntimeWritePolicy runtime_policy;
    std::vector<ChannelState> channels;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
};

ControlLoop::ControlLoop(ControlConfig base_config,
                         ControlLoopConfig loop_config,
                         std::filesystem::path runtime_home)
    : impl_(std::make_unique<Impl>()) {
    impl_->base = std::move(base_config);
    impl_->loop = std::move(loop_config);
    impl_->runtime_home = std::move(runtime_home);
    impl_->runtime_policy = ResolveRuntimeWritePolicy(&impl_->base);
    impl_->channels.reserve(impl_->loop.channels.size());
    for (const auto& ch_cfg : impl_->loop.channels) {
        ChannelState state;
        state.config = ch_cfg;
        impl_->channels.push_back(std::move(state));
    }
}

ControlLoop::~ControlLoop() = default;

namespace {

bool WriteLoopStatus(const std::filesystem::path& runtime_home,
                     const std::string& mode_label,
                     const std::string& status,
                     const std::string& status_detail,
                     std::uint64_t tick_count,
                     const std::string& last_evaluation_iso,
                     const std::vector<ChannelState>& channels) {
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) return false;

    const std::filesystem::path target = runtime_home / "control_runtime.json";
    const std::filesystem::path temp = runtime_home / "control_runtime.json.tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) return false;
        stream << "{\n"
               << "  \"schema_version\": 2,\n"
               << "  \"mode\": \"" << JsonEscape(mode_label) << "\",\n"
               << "  \"status\": \"" << JsonEscape(status) << "\",\n"
               << "  \"status_detail\": \"" << JsonEscape(status_detail) << "\",\n"
               << "  \"loop_tick_count\": " << tick_count << ",\n"
               << "  \"loop_last_evaluation\": \"" << JsonEscape(last_evaluation_iso) << "\",\n"
               << "  \"controlled_channels\": [";
        for (std::size_t i = 0u; i < channels.size(); ++i) {
            const ChannelState& ch = channels[i];
            if (i > 0u) stream << ",";
            stream << "\n    {\n"
                   << "      \"channel\": " << ch.config.channel << ",\n"
                   << "      \"total_writes\": " << ch.total_writes << ",\n"
                   << "      \"last_setpoint_pct\": "
                   << (std::isnan(ch.last_setpoint_pct) ? 0.0 : ch.last_setpoint_pct) << ",\n"
                   << "      \"last_observed_temp_c\": "
                   << (std::isnan(ch.last_observed_temp_c) ? 0.0 : ch.last_observed_temp_c) << ",\n"
                   << "      \"baseline_captured\": "
                   << (ch.baseline_captured ? "true" : "false") << "\n"
                   << "    }";
        }
        if (!channels.empty()) stream << "\n  ";
        stream << "]\n}\n";
        stream.flush();
        if (stream.fail()) return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace

int ControlLoop::RunUntilStopped(const std::atomic<bool>& stop_flag) {
    AmdReader amd_reader;
    GpuReader gpu_reader;

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(impl_->runtime_policy);
    } catch (const std::exception& error) {
        WriteLoopStatus(impl_->runtime_home, "control-loop", "failed",
                        std::string("direct writer init failed: ") + error.what(),
                        0u, FormatLocalIso8601(std::chrono::system_clock::now()),
                        impl_->channels);
        return 1;
    }

    std::uint64_t tick_count = 0u;
    {
        std::ostringstream detail;
        detail << "direct telemetry active; fan_writer="
               << fan_writer->BackendLabel()
               << " policy="
               << (impl_->runtime_policy.present
                       ? impl_->runtime_policy.source_path.string()
                       : std::string("(none)"))
               << " poll_tick_ms="
               << impl_->loop.poll_tick_ms
               << " cooldown_ms=" << impl_->loop.write_cooldown_ms
               << " deadband_pct=" << impl_->loop.deadband_pct
               << " hold_ms=" << impl_->loop.control_hold_ms
               << " channels=" << impl_->channels.size() << ")";
        WriteLoopStatus(impl_->runtime_home, "control-loop", "running",
                        detail.str(), tick_count,
                        FormatLocalIso8601(std::chrono::system_clock::now()),
                        impl_->channels);
    }

    while (!stop_flag.load()) {
        ++tick_count;
        const auto now_steady = std::chrono::steady_clock::now();
        const auto eval_iso = FormatLocalIso8601(std::chrono::system_clock::now());

        RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *fan_writer, impl_->runtime_policy);
        const bool runtime_snapshot_available =
            RuntimeSnapshotHasTelemetry(runtime_snapshot);

        // Extract CPU temp.
        TempInputs temp_inputs;
        if (runtime_snapshot_available) {
            const double cpu_c = FindRuntimeAmdSensorTemperature(
                runtime_snapshot, impl_->loop.cpu_temp_label);
            if (!std::isnan(cpu_c)) {
                temp_inputs.cpu_c = cpu_c;
                temp_inputs.cpu_available = true;
                temp_inputs.cpu_label = impl_->loop.cpu_temp_label;
            }
        }
        if (runtime_snapshot.gpu.available) {
            temp_inputs.gpu_c = (std::max)(runtime_snapshot.gpu.core_c,
                                           runtime_snapshot.gpu.memjn_c);
            temp_inputs.gpu_available = true;
            temp_inputs.gpu_label = "gpu_max";
        }

        if (runtime_snapshot_available) {
            WriteRuntimeSnapshotFile(impl_->runtime_home, runtime_snapshot);
        }

        // Per-channel decisions.
        for (auto& channel : impl_->channels) {
            const std::uint32_t effective_cooldown_ms =
                EffectiveWriteCooldownMs(impl_->loop, channel.config);
            const double effective_deadband_pct =
                EffectiveDeadbandPct(impl_->loop, channel.config);
            const std::uint32_t effective_hold_ms =
                EffectiveControlHoldMs(impl_->loop, channel.config);

            if (!channel.baseline_captured) {
                if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                        runtime_snapshot, channel.config.channel)) {
                    channel.baseline_duty_raw = fan->duty_raw;
                    channel.baseline_mode_raw = fan->mode_raw;
                    channel.baseline_captured = true;
                }
            }

            if (channel.write_active &&
                effective_hold_ms > 0u &&
                now_steady >= channel.hold_deadline &&
                channel.baseline_captured) {
                const FanWriteResult restore_result =
                    fan_writer->RestoreSavedState(channel.config.channel,
                                                  channel.baseline_duty_raw,
                                                  channel.baseline_mode_raw);
                if (restore_result) {
                    channel.write_active = false;
                    channel.last_issued_pct =
                        std::numeric_limits<double>::quiet_NaN();
                    try {
                        RemovePendingWrite(impl_->runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception&) {
                        // Best-effort; stale sidecar can still be reconciled
                        // on a future startup.
                    }
                }
            }

            const double blended = BlendTemps(temp_inputs, channel.config.temp_blend);
            channel.last_observed_temp_c = blended;
            if (blended < -100.0) continue;  // no valid input

            const double setpoint = LookupCurve(channel.config.curve, blended,
                                                channel.config.min_duty_pct);
            channel.last_setpoint_pct = setpoint;

            if (!std::isnan(channel.last_issued_pct)) {
                const double delta = std::abs(setpoint - channel.last_issued_pct);
                if (delta < effective_deadband_pct) continue;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_steady - channel.last_write_time).count();
            if (std::isnan(channel.last_issued_pct)) {
                // First write for this channel — allow immediately.
            } else if (static_cast<std::uint64_t>(elapsed) <
                       static_cast<std::uint64_t>(effective_cooldown_ms)) {
                continue;
            }

            // Ensure we have a baseline before recording the sidecar.
            if (!channel.baseline_captured) {
                continue;  // skip until snapshot provides baseline
            }

            if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                    runtime_snapshot, channel.config.channel)) {
                if (!fan->effective_write_allowed) {
                    continue;
                }
            }

            // Record sidecar entry.
            PendingWriteEntry entry;
            entry.channel = channel.config.channel;
            entry.baseline_duty_raw = channel.baseline_duty_raw;
            entry.baseline_mode_raw = channel.baseline_mode_raw;
            entry.target_pct = setpoint;
            entry.requested_hold_ms = effective_hold_ms;
            entry.started_iso = eval_iso;
            entry.child_pid = 0u;
            try {
                UpsertPendingWrite(impl_->runtime_home, entry);
            } catch (const std::exception&) {
                continue;
            }

            const FanWriteResult write_result =
                fan_writer->ApplyDuty(channel.config.channel, setpoint);
            if (!write_result) {
                if (write_result.error == FanWriteError::kPolicyRefused) {
                    try {
                        RemovePendingWrite(impl_->runtime_home,
                                           channel.config.channel);
                    } catch (const std::exception&) {
                        // Best-effort.
                    }
                }
                continue;
            }
            channel.write_active = true;
            channel.hold_deadline =
                effective_hold_ms == 0u
                    ? std::chrono::steady_clock::time_point::max()
                    : now_steady + std::chrono::milliseconds(effective_hold_ms);
            channel.last_issued_pct = setpoint;
            channel.last_write_time = now_steady;
            ++channel.total_writes;
        }

        {
            std::ostringstream td;
            td << "tick poll_tick_ms=" << impl_->loop.poll_tick_ms
               << " cooldown=" << impl_->loop.write_cooldown_ms
               << " deadband=" << impl_->loop.deadband_pct;
            WriteLoopStatus(impl_->runtime_home, "control-loop", "running",
                            td.str(), tick_count, eval_iso, impl_->channels);
        }

        std::unique_lock<std::mutex> lock(impl_->wake_mutex);
        impl_->wake_cv.wait_for(
            lock,
            std::chrono::milliseconds(impl_->loop.poll_tick_ms),
            [&stop_flag] { return stop_flag.load(); });
    }

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteLoopStatus(impl_->runtime_home, "control-loop", "shutdown",
                    "stop requested", tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    impl_->channels);
    bool restore_failure = false;
    for (auto& channel : impl_->channels) {
        if (channel.write_active && channel.baseline_captured) {
            const FanWriteResult restore_result =
                fan_writer->RestoreSavedState(channel.config.channel,
                                              channel.baseline_duty_raw,
                                              channel.baseline_mode_raw);
            if (!restore_result) {
                restore_failure = true;
                continue;
            }
        }
        try {
            RemovePendingWrite(impl_->runtime_home, channel.config.channel);
        } catch (const std::exception&) {
            restore_failure = true;
        }
    }
    WriteLoopStatus(impl_->runtime_home, "control-loop", "shutdown",
                    restore_failure ? "restore failed"
                                    : "channels restored",
                    tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    impl_->channels);
    return restore_failure ? 1 : 0;
}

}  // namespace svg_mb_control
