#include "write_orchestrator.h"

#include "bench_bridge.h"
#include "pending_writes.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <chrono>

namespace svg_mb_control {

namespace {

std::size_t SkipWhitespace(const std::string& text, std::size_t offset,
                           std::size_t limit) {
    while (offset < limit &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
        ++offset;
    }
    return offset;
}

std::optional<bool> FindTopLevelBoolField(const std::string& text,
                                          std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token);
    if (key_offset == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t value_start =
        SkipWhitespace(text, colon + 1u, text.size());
    if (value_start + 4u <= text.size() &&
        text.compare(value_start, 4u, "true") == 0) {
        return true;
    }
    if (value_start + 5u <= text.size() &&
        text.compare(value_start, 5u, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> FindTopLevelStringField(const std::string& text,
                                                   std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token);
    if (key_offset == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t value_start =
        SkipWhitespace(text, colon + 1u, text.size());
    if (value_start >= text.size() || text[value_start] != '"') {
        return std::nullopt;
    }
    std::string output;
    for (std::size_t index = value_start + 1u; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\\') {
            if (index + 1u >= text.size()) {
                return std::nullopt;
            }
            const char escaped = text[++index];
            switch (escaped) {
                case '\\': output.push_back('\\'); break;
                case '"': output.push_back('"'); break;
                case '/': output.push_back('/'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return std::nullopt;
            }
            continue;
        }
        if (ch == '"') {
            return output;
        }
        output.push_back(ch);
    }
    return std::nullopt;
}

// Find the [..] range that is the value of the `fans` top-level field.
std::pair<std::size_t, std::size_t> FindFansArrayRange(
    const std::string& text) {
    const std::string token = "\"fans\"";
    const std::size_t key_offset = text.find(token);
    if (key_offset == std::string::npos) {
        throw std::runtime_error("snapshot missing fans array.");
    }
    const std::size_t bracket_open = text.find('[', key_offset + token.size());
    if (bracket_open == std::string::npos) {
        throw std::runtime_error("snapshot fans field is not an array.");
    }
    // Scan for matching close bracket, ignoring content inside strings.
    std::size_t depth = 1u;
    std::size_t cursor = bracket_open + 1u;
    bool in_string = false;
    while (cursor < text.size() && depth > 0u) {
        const char ch = text[cursor];
        if (in_string) {
            if (ch == '\\' && cursor + 1u < text.size()) {
                cursor += 2u;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0u) {
                return {bracket_open + 1u, cursor};
            }
        }
        ++cursor;
    }
    throw std::runtime_error("snapshot fans array not terminated.");
}

std::optional<double> FindNumericInRange(const std::string& text,
                                         std::size_t range_begin,
                                         std::size_t range_end,
                                         std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::nullopt;
    }
    std::size_t value_start = SkipWhitespace(text, colon + 1u, range_end);
    std::size_t value_end = value_start;
    while (value_end < range_end &&
           (std::isdigit(static_cast<unsigned char>(text[value_end])) != 0 ||
            text[value_end] == '.' || text[value_end] == '-' ||
            text[value_end] == '+' || text[value_end] == 'e' ||
            text[value_end] == 'E')) {
        ++value_end;
    }
    if (value_end == value_start) {
        return std::nullopt;
    }
    try {
        return std::stod(text.substr(value_start, value_end - value_start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<bool> FindBoolInRange(const std::string& text,
                                    std::size_t range_begin,
                                    std::size_t range_end,
                                    std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t key_offset = text.find(token, range_begin);
    if (key_offset == std::string::npos || key_offset >= range_end) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', key_offset + token.size());
    if (colon == std::string::npos || colon >= range_end) {
        return std::nullopt;
    }
    const std::size_t value_start = SkipWhitespace(text, colon + 1u, range_end);
    if (value_start + 4u <= range_end &&
        text.compare(value_start, 4u, "true") == 0) {
        return true;
    }
    if (value_start + 5u <= range_end &&
        text.compare(value_start, 5u, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

// Iterates each `{...}` object inside the fans array and invokes `visit`.
// `visit` receives (object_begin, object_end_exclusive_of_close_brace).
template <class F>
void ForEachFanObject(const std::string& text,
                      std::size_t array_begin,
                      std::size_t array_end,
                      F&& visit) {
    std::size_t cursor = array_begin;
    while (cursor < array_end) {
        const std::size_t obj_open = text.find('{', cursor);
        if (obj_open == std::string::npos || obj_open >= array_end) {
            break;
        }
        std::size_t depth = 1u;
        std::size_t obj_close = obj_open + 1u;
        bool in_string = false;
        while (obj_close < array_end && depth > 0u) {
            const char ch = text[obj_close];
            if (in_string) {
                if (ch == '\\' && obj_close + 1u < array_end) {
                    obj_close += 2u;
                    continue;
                }
                if (ch == '"') {
                    in_string = false;
                }
                ++obj_close;
                continue;
            }
            if (ch == '"') {
                in_string = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0u) {
                    break;
                }
            }
            ++obj_close;
        }
        if (depth != 0u) {
            throw std::runtime_error("snapshot fan object not terminated.");
        }
        visit(obj_open, obj_close);
        cursor = obj_close + 1u;
    }
}

std::optional<std::chrono::system_clock::time_point> ParseSnapshotLocalTime(
    const std::string& iso_text) {
    // Expect format "%Y-%m-%dT%H:%M:%S" as local time.
    std::tm tm_value{};
    std::istringstream stream(iso_text);
    stream >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm_value.tm_isdst = -1;
    const std::time_t tt = std::mktime(&tm_value);
    if (tt == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(tt);
}

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    if (written == 0u) {
        return {};
    }
    return std::string(buffer.data(), written);
}

}  // namespace

BaselineCapture CaptureBaselineFromSnapshotJson(
    const std::string& snapshot_json,
    std::uint32_t channel,
    std::uint32_t freshness_ceiling_ms,
    std::chrono::system_clock::time_point now) {
    BaselineCapture result;

    if (const auto policy_writes = FindTopLevelBoolField(
            snapshot_json, "policy_writes_enabled")) {
        result.policy_writes_enabled_present = true;
        result.policy_writes_enabled = *policy_writes;
    }

    const auto snapshot_time_text =
        FindTopLevelStringField(snapshot_json, "snapshot_time");
    if (snapshot_time_text) {
        result.snapshot_time_iso = *snapshot_time_text;
        if (const auto parsed = ParseSnapshotLocalTime(*snapshot_time_text)) {
            const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *parsed).count();
            result.snapshot_age_ms = delta;
            result.freshness_ok = delta >= 0 &&
                static_cast<std::uint64_t>(delta) <=
                    static_cast<std::uint64_t>(freshness_ceiling_ms);
        }
    }

    const auto [fans_begin, fans_end] = FindFansArrayRange(snapshot_json);
    ForEachFanObject(
        snapshot_json, fans_begin, fans_end,
        [&](std::size_t obj_open, std::size_t obj_close) {
            const auto channel_value =
                FindNumericInRange(snapshot_json, obj_open, obj_close, "channel");
            if (!channel_value) {
                return;
            }
            if (static_cast<std::uint32_t>(*channel_value) != channel) {
                return;
            }
            result.present = true;
            result.channel = channel;
            const auto duty =
                FindNumericInRange(snapshot_json, obj_open, obj_close, "duty_raw");
            const auto mode =
                FindNumericInRange(snapshot_json, obj_open, obj_close, "mode_raw");
            const auto write_allowed = FindBoolInRange(
                snapshot_json, obj_open, obj_close, "write_allowed");
            const auto policy_blocked = FindBoolInRange(
                snapshot_json, obj_open, obj_close, "policy_blocked");
            const auto effective = FindBoolInRange(
                snapshot_json, obj_open, obj_close, "effective_write_allowed");
            if (duty) result.duty_raw = static_cast<std::uint8_t>(*duty);
            if (mode) result.mode_raw = static_cast<std::uint8_t>(*mode);
            if (write_allowed) result.write_allowed = *write_allowed;
            if (policy_blocked) result.policy_blocked = *policy_blocked;
            if (effective) result.effective_write_allowed = *effective;
        });

    return result;
}

int RunWriteOnce(const ControlConfig& config,
                 const std::wstring& bench_exe_path,
                 const std::filesystem::path& runtime_home,
                 const WriteRequest& request,
                 const std::atomic<bool>& stop_flag) {
    if (request.target_pct < 0.0 || request.target_pct > 100.0) {
        std::cerr << "Error: --write-pct must be in [0, 100]" << '\n';
        return 2;
    }

    // Step 1: fresh snapshot via read-snapshot.
    JsonArtifactLaunchResult launch;
    try {
        launch = RunReadSnapshot(bench_exe_path, 15000u);
    } catch (const std::exception& error) {
        std::cerr << "Error: read-snapshot failed: " << error.what() << '\n';
        return 1;
    }
    std::string snapshot_json;
    try {
        snapshot_json = LoadJsonObjectFile(launch.json_artifact_path);
    } catch (const std::exception& error) {
        std::cerr << "Error: failed to load snapshot: " << error.what() << '\n';
        return 1;
    }

    // Step 2: baseline capture + policy check.
    const BaselineCapture baseline = CaptureBaselineFromSnapshotJson(
        snapshot_json, request.channel,
        config.baseline_freshness_ceiling_ms,
        std::chrono::system_clock::now());

    if (!baseline.present) {
        std::cerr << "Error: channel " << request.channel
                  << " not present in snapshot fans array" << '\n';
        return 3;
    }
    if (!baseline.freshness_ok) {
        std::cerr << "Error: snapshot age "
                  << baseline.snapshot_age_ms
                  << " ms exceeds baseline_freshness_ceiling_ms="
                  << config.baseline_freshness_ceiling_ms
                  << " (snapshot_time=\"" << baseline.snapshot_time_iso
                  << "\")" << '\n';
        return 4;
    }
    if (!baseline.effective_write_allowed) {
        std::cerr << "Error: channel " << request.channel
                  << " effective_write_allowed=false"
                  << " (write_allowed=" << (baseline.write_allowed ? "true" : "false")
                  << " policy_blocked=" << (baseline.policy_blocked ? "true" : "false")
                  << ")" << '\n';
        return 5;
    }
    if (baseline.policy_writes_enabled_present &&
        !baseline.policy_writes_enabled) {
        std::cerr << "Error: runtime policy has writes_enabled=false; "
                  << "no write attempted." << '\n';
        return 6;
    }

    // Step 3: record sidecar entry BEFORE spawning.
    PendingWriteEntry entry;
    entry.channel = request.channel;
    entry.baseline_duty_raw = baseline.duty_raw;
    entry.baseline_mode_raw = baseline.mode_raw;
    entry.target_pct = request.target_pct;
    entry.requested_hold_ms = request.hold_ms;
    entry.bench_started_iso =
        FormatLocalIso8601(std::chrono::system_clock::now());
    entry.bench_child_pid = 0u;

    try {
        UpsertPendingWrite(runtime_home, entry);
    } catch (const std::exception& error) {
        std::cerr << "Error: failed to record pending-writes sidecar: "
                  << error.what() << '\n';
        return 1;
    }

    // Step 4: spawn set-fixed-duty child.
    std::ostringstream pct_stream;
    pct_stream << request.target_pct;
    const std::string pct_narrow = pct_stream.str();
    const std::wstring pct_wide(pct_narrow.begin(), pct_narrow.end());
    std::vector<std::wstring> args = {
        L"set-fixed-duty",
        L"--channel", std::to_wstring(request.channel),
        L"--pct", pct_wide,
        L"--hold-ms", std::to_wstring(request.hold_ms),
    };

    BenchChildSupervisor supervisor(bench_exe_path, args);
    try {
        supervisor.Start();
    } catch (const std::exception& error) {
        std::cerr << "Error: set-fixed-duty failed to start: "
                  << error.what() << '\n';
        return 1;
    }

    // Step 5: block until child exit, responding to stop_flag.
    bool stopped_by_signal = false;
    while (supervisor.IsRunning()) {
        if (stop_flag.load()) {
            supervisor.RequestStop(2000u);
            stopped_by_signal = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int exit_code = supervisor.LastExitCode();
    // If the child was stopped via CTRL_BREAK, treat that as clean exit
    // from Control's perspective: the channel is back in auto mode via
    // set-fixed-duty's own restore path.
    if (stopped_by_signal) {
        try {
            RemovePendingWrite(runtime_home, request.channel);
        } catch (const std::exception& error) {
            std::cerr << "Warning: signal stop completed but sidecar clear failed: "
                      << error.what() << '\n';
            return 1;
        }
        return 0;
    }

    // Step 6: outcome.
    if (exit_code == 0) {
        try {
            RemovePendingWrite(runtime_home, request.channel);
        } catch (const std::exception& error) {
            std::cerr << "Warning: set-fixed-duty completed but sidecar clear failed: "
                      << error.what() << '\n';
            return 1;
        }
        return 0;
    }

    const std::string stderr_tail = supervisor.StderrTail();
    std::cerr << "Error: set-fixed-duty exited with code " << exit_code << '\n';
    if (!stderr_tail.empty()) {
        std::cerr << stderr_tail;
        if (stderr_tail.back() != '\n') {
            std::cerr << '\n';
        }
    }
    // Exit code 2 from Bench is a parse-time write-policy refusal: no fan
    // register was touched. Clear the sidecar so reconciliation does not try
    // to restore a channel that was never modified.
    // See SVG-MB-Bench/docs/BRIDGE_CONTRACT.md "Exit Code Rule".
    if (exit_code == 2) {
        try {
            RemovePendingWrite(runtime_home, request.channel);
        } catch (const std::exception& error) {
            std::cerr << "Warning: policy-refusal sidecar clear failed: "
                      << error.what() << '\n';
        }
    }
    return exit_code;
}

int ReconcilePendingWrites(const std::wstring& bench_exe_path,
                           const std::filesystem::path& runtime_home,
                           std::uint32_t restore_timeout_ms) {
    std::vector<PendingWriteEntry> entries;
    try {
        entries = ReadPendingWrites(runtime_home);
    } catch (const std::exception& error) {
        std::cerr << "Error: could not read pending-writes sidecar: "
                  << error.what() << '\n';
        return 1;
    }
    if (entries.empty()) {
        return 0;
    }

    bool any_failure = false;
    for (const auto& entry : entries) {
        const std::vector<std::wstring> args = {
            L"restore-auto",
            L"--channel", std::to_wstring(entry.channel),
            L"--saved-duty-raw", std::to_wstring(entry.baseline_duty_raw),
            L"--saved-mode-raw", std::to_wstring(entry.baseline_mode_raw),
        };
        BridgeProcessResult result;
        try {
            result = RunBenchProcess(bench_exe_path, args, restore_timeout_ms);
        } catch (const std::exception& error) {
            std::cerr << "Error: restore-auto launch failed for channel "
                      << entry.channel << ": " << error.what() << '\n';
            any_failure = true;
            continue;
        }
        if (result.exit_code != 0) {
            std::cerr << "Error: restore-auto exit " << result.exit_code
                      << " for channel " << entry.channel << '\n';
            if (!result.stderr_text.empty()) {
                std::cerr << result.stderr_text;
            }
            any_failure = true;
            continue;
        }
        try {
            RemovePendingWrite(runtime_home, entry.channel);
        } catch (const std::exception& error) {
            std::cerr << "Error: restore-auto succeeded but sidecar remove failed: "
                      << error.what() << '\n';
            any_failure = true;
        }
    }
    return any_failure ? 1 : 0;
}

}  // namespace svg_mb_control
