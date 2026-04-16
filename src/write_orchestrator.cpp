#include "write_orchestrator.h"

#include "amd_reader.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#include <array>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <chrono>
#include <thread>

namespace svg_mb_control {

namespace {

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
    const RuntimeSnapshot snapshot = ParseRuntimeSnapshotJson(snapshot_json);

    result.policy_writes_enabled_present = snapshot.policy_writes_enabled_present;
    result.policy_writes_enabled = snapshot.policy_writes_enabled;
    result.snapshot_time_iso = snapshot.snapshot_time_iso;
    if (!snapshot.snapshot_time_iso.empty()) {
        if (const auto parsed = ParseSnapshotLocalTime(snapshot.snapshot_time_iso)) {
            const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *parsed).count();
            result.snapshot_age_ms = delta;
            result.freshness_ok = delta >= 0 &&
                static_cast<std::uint64_t>(delta) <=
                    static_cast<std::uint64_t>(freshness_ceiling_ms);
        }
    }

    if (const RuntimeFanSnapshot* fan =
            FindRuntimeFanChannel(snapshot, channel)) {
        result.present = true;
        result.snapshot_channel_present = true;
        result.channel = channel;
        result.duty_raw = fan->duty_raw;
        result.mode_raw = fan->mode_raw;
        result.write_allowed = fan->write_allowed;
        result.policy_blocked = fan->policy_blocked;
        result.effective_write_allowed = fan->effective_write_allowed;
    }

    return result;
}

int RunWriteOnce(const ControlConfig& config,
                 const std::filesystem::path& runtime_home,
                 const WriteRequest& request,
                 const std::atomic<bool>& stop_flag) {
    if (request.target_pct < 0.0 || request.target_pct > 100.0) {
        std::cerr << "Error: --write-pct must be in [0, 100]" << '\n';
        return 2;
    }

    // Step 1: initialize the in-process write backend and sample a fresh
    // direct runtime snapshot for baseline capture.
    const RuntimeWritePolicy runtime_policy = ResolveRuntimeWritePolicy(&config);

    try {
        std::unique_ptr<FanWriter> writer =
            CreateFanWriter(runtime_policy);
        AmdReader amd_reader;
        GpuReader gpu_reader;
        const RuntimeSnapshot direct_snapshot = SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *writer, runtime_policy);
        const std::string snapshot_json =
            SerializeRuntimeSnapshotJson(direct_snapshot);

        // Step 2: baseline capture + policy check.
        BaselineCapture baseline = CaptureBaselineFromSnapshotJson(
            snapshot_json, request.channel,
            config.baseline_freshness_ceiling_ms,
            std::chrono::system_clock::now());

        if (!baseline.freshness_ok) {
            std::cerr << "Error: snapshot age "
                      << baseline.snapshot_age_ms
                      << " ms exceeds baseline_freshness_ceiling_ms="
                      << config.baseline_freshness_ceiling_ms
                      << " (snapshot_time=\"" << baseline.snapshot_time_iso
                      << "\")" << '\n';
            return 4;
        }
        if (baseline.policy_writes_enabled_present &&
            !baseline.policy_writes_enabled) {
            std::cerr << "Error: runtime policy has writes_enabled=false; "
                      << "no write attempted." << '\n';
            return 6;
        }
        if (!baseline.present) {
            std::cerr << "Error: channel " << request.channel
                      << " not present in direct fan state" << '\n';
            return 3;
        }
        if (baseline.snapshot_channel_present &&
            !baseline.effective_write_allowed) {
            std::cerr << "Error: channel " << request.channel
                      << " effective_write_allowed=false"
                      << " (write_allowed="
                      << (baseline.write_allowed ? "true" : "false")
                      << " policy_blocked="
                      << (baseline.policy_blocked ? "true" : "false")
                      << ")" << '\n';
            return 5;
        }

        // Step 3: record sidecar entry BEFORE touching hardware.
        PendingWriteEntry entry;
        entry.channel = request.channel;
        entry.baseline_duty_raw = baseline.duty_raw;
        entry.baseline_mode_raw = baseline.mode_raw;
        entry.target_pct = request.target_pct;
        entry.requested_hold_ms = request.hold_ms;
        entry.started_iso =
            FormatLocalIso8601(std::chrono::system_clock::now());
        entry.child_pid = 0u;

        try {
            UpsertPendingWrite(runtime_home, entry);
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to record pending-writes sidecar: "
                      << error.what() << '\n';
            return 1;
        }

        // Step 4: apply the fixed duty in-process.
        const FanWriteResult write_result =
            writer->ApplyDuty(request.channel, request.target_pct);
        if (!write_result) {
            std::cerr << "Error: direct write failed: "
                      << write_result.detail << '\n';
            if (write_result.error == FanWriteError::kPolicyRefused) {
                try {
                    RemovePendingWrite(runtime_home, request.channel);
                } catch (const std::exception& error) {
                    std::cerr
                        << "Warning: policy-refusal sidecar clear failed: "
                        << error.what() << '\n';
                }
                return 2;
            }
            return 1;
        }

        // Step 5: hold until timeout or stop request, then restore baseline.
        const auto hold_deadline =
            request.hold_ms == 0u
                ? std::chrono::steady_clock::time_point::max()
                : std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(request.hold_ms);
        while (true) {
            if (stop_flag.load()) {
                break;
            }
            if (request.hold_ms > 0u &&
                std::chrono::steady_clock::now() >= hold_deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const FanWriteResult restore_result = writer->RestoreSavedState(
            request.channel, baseline.duty_raw, baseline.mode_raw);
        if (!restore_result) {
            std::cerr << "Error: restore failed after direct write: "
                      << restore_result.detail << '\n';
            return 1;
        }

        try {
            RemovePendingWrite(runtime_home, request.channel);
        } catch (const std::exception& error) {
            std::cerr
                << "Warning: direct write completed but sidecar clear failed: "
                << error.what() << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: direct writer init failed: "
                  << error.what() << '\n';
        return 1;
    }
}

int ReconcilePendingWrites(const std::filesystem::path& runtime_home,
                           const RuntimeWritePolicy& runtime_policy,
                           std::uint32_t restore_timeout_ms) {
    (void)restore_timeout_ms;

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

    std::unique_ptr<FanWriter> writer;
    try {
        writer = CreateFanWriter(runtime_policy);
    } catch (const std::exception& error) {
        std::cerr << "Error: direct writer init failed during reconciliation: "
                  << error.what() << '\n';
        return 1;
    }

    bool any_failure = false;
    for (const auto& entry : entries) {
        const FanWriteResult restore_result = writer->RestoreSavedState(
            entry.channel, entry.baseline_duty_raw, entry.baseline_mode_raw);
        if (!restore_result) {
            std::cerr << "Error: reconcile restore failed for channel "
                      << entry.channel << ": "
                      << restore_result.detail << '\n';
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
