#include "read_loop.h"

#include "amd_reader.h"
#include "control_scheduler.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "runtime_artifacts.h"
#include "runtime_logging.h"
#include "runtime_lifecycle.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace svg_mb_control {

namespace {

std::filesystem::path CurrentExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data(),
                                 buffer.data() + length).parent_path();
}

bool WriteRuntimeStatusFile(const std::filesystem::path& runtime_home,
                            const ReadLoop::Status& status) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["status"] = status.status;
    payload["mode"] = "read-loop";
    payload["process_id"] = GetCurrentProcessId();
    payload["status_detail"] = status.status_detail;
    payload["last_refresh"] = status.last_refresh_iso;
    payload["snapshot_source"] = status.snapshot_source;
    payload["restart_count"] = status.restart_count;
    payload["skipped_polls"] = status.skipped_polls;
    payload["successful_polls"] = status.successful_polls;
    payload["stale"] = status.stale;
    payload["child_pid"] = status.child_pid;
    payload["log_csv_path"] = status.log_csv_path;
    payload["log_manifest_path"] = status.log_manifest_path;
    payload["event_log_path"] = status.event_log_path;
    return TryWriteJsonFileAtomic(runtime_home / "control_runtime.json",
                                  payload);
}

std::uint32_t ResolveStalenessThresholdMs(const ControlConfig& config) {
    if (config.staleness_threshold_ms > 0u) {
        return config.staleness_threshold_ms;
    }
    const std::uint32_t poll = config.poll_ms > 0u ? config.poll_ms : 1000u;
    return poll * 3u;
}

}  // namespace

std::filesystem::path ResolveRuntimeHomePath(const ControlConfig& config) {
    if (!config.runtime_home_path.empty()) {
        return config.runtime_home_path;
    }
    const std::filesystem::path exe_dir = CurrentExecutableDirectory();
    if (exe_dir.empty()) {
        return std::filesystem::current_path() / "runtime";
    }
    return exe_dir / "runtime";
}

struct ReadLoop::Impl {
    ControlConfig config;
    std::filesystem::path runtime_home;

    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    std::atomic<bool> stop_requested{false};
};

ReadLoop::ReadLoop(ControlConfig config, std::filesystem::path runtime_home)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->runtime_home = std::move(runtime_home);
}

ReadLoop::~ReadLoop() = default;

void ReadLoop::RequestStop() {
    impl_->stop_requested.store(true);
    std::lock_guard<std::mutex> lock(impl_->wake_mutex);
    impl_->wake_cv.notify_all();
}

int ReadLoop::RunUntilStopped() {
    const std::uint32_t poll_ms = impl_->config.poll_ms > 0u
                                      ? impl_->config.poll_ms
                                      : 1000u;
    const std::uint32_t staleness_threshold_ms =
        ResolveStalenessThresholdMs(impl_->config);

    std::error_code ec;
    std::filesystem::create_directories(impl_->runtime_home, ec);
    ClearRuntimeStopRequest(impl_->runtime_home);

    Status status;
    status.status = "running";
    status.status_detail = "starting";
    status.snapshot_source = "direct-runtime-snapshot";
    status.event_log_path =
        ResolveRuntimeEventLogPath(impl_->runtime_home).string();

    auto publish_status = [&](const std::string& state,
                              const std::string& detail) {
        status.status = state;
        status.status_detail = detail;
        WriteRuntimeStatusFile(impl_->runtime_home, status);
    };

    RuntimeCsvLogger csv_logger(
        impl_->runtime_home,
        impl_->config.log_rotate_hours,
        impl_->config.log_retain_days,
        impl_->config.csv_flush_interval_rows);
    if (csv_logger.Open("read-loop", BuildReadLoopCsvHeader())) {
        status.log_csv_path = csv_logger.active_archive_path().string();
        status.log_manifest_path = csv_logger.manifest_path().string();
    }

    publish_status("running", "initializing direct readers");

    const RuntimeWritePolicy runtime_policy =
        ResolveRuntimeWritePolicy(&impl_->config);
    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(runtime_policy);
    } catch (const std::exception& error) {
        AppendRuntimeEvent(
            impl_->runtime_home,
            RuntimeLogEvent{
                .mode = "read-loop",
                .event_type = "read_loop.init_failed",
                .detail = std::string("direct reader init failed: ") +
                          error.what(),
                .success = false,
                .log_csv_path = status.log_csv_path,
                .event_log_path = status.event_log_path,
            });
        publish_status("direct-read-failed",
                       std::string("direct reader init failed: ") +
                           error.what());
        return 1;
    }

    std::ostringstream start_detail;
    start_detail << "read-loop started"
                 << " poll_ms=" << poll_ms
                 << " staleness_threshold_ms=" << staleness_threshold_ms
                 << " snapshot_mirror_configured="
                 << (!impl_->config.snapshot_path.empty() ? "true" : "false");
    if (!impl_->config.snapshot_path.empty()) {
        start_detail << " snapshot_path=" << impl_->config.snapshot_path.string();
    }
    AppendRuntimeEvent(
        impl_->runtime_home,
        RuntimeLogEvent{
            .mode = "read-loop",
            .event_type = "read_loop.start",
            .detail = start_detail.str(),
            .success = true,
            .log_csv_path = status.log_csv_path,
            .event_log_path = status.event_log_path,
            .successful_polls = status.successful_polls,
            .skipped_polls = status.skipped_polls,
            .stale = status.stale,
            .snapshot_mirror_configured =
                !impl_->config.snapshot_path.empty(),
        });

    AmdReader amd_reader;
    GpuReader gpu_reader;
    auto last_success_time = std::chrono::steady_clock::now();

    while (!impl_->stop_requested.load() &&
           !RuntimeStopRequested(impl_->runtime_home)) {
        try {
            RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
                amd_reader, gpu_reader, *fan_writer, runtime_policy);

            if (csv_logger.MaybeRotate()) {
                status.log_csv_path =
                    csv_logger.active_archive_path().string();
                status.log_manifest_path =
                    csv_logger.manifest_path().string();
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "read-loop",
                        .event_type = "read_loop.log_rotated",
                        .detail = "telemetry log rotated",
                        .success = true,
                        .log_csv_path = status.log_csv_path,
                        .event_log_path = status.event_log_path,
                        .successful_polls = status.successful_polls,
                        .skipped_polls = status.skipped_polls,
                        .stale = status.stale,
                    });
            }

            const bool telemetry_available =
                RuntimeSnapshotHasTelemetry(runtime_snapshot);
            const bool runtime_home_published = WriteRuntimeSnapshotFile(
                impl_->runtime_home, runtime_snapshot);
            const bool snapshot_mirror_configured =
                !impl_->config.snapshot_path.empty();
            bool snapshot_mirror_published = false;
            if (!impl_->config.snapshot_path.empty()) {
                snapshot_mirror_published = WriteRuntimeSnapshotJsonFile(
                    impl_->config.snapshot_path,
                    runtime_snapshot);
            }
            const bool wrote_outputs =
                runtime_home_published &&
                (!snapshot_mirror_configured || snapshot_mirror_published);

            if (wrote_outputs && telemetry_available) {
                last_success_time = std::chrono::steady_clock::now();
                ++status.successful_polls;
                status.stale = false;
                status.last_refresh_iso = FormatLocalIso8601(
                    std::chrono::system_clock::now());
                status.status_detail = "direct sample refreshed";
            } else {
                ++status.skipped_polls;
                if (!telemetry_available) {
                    status.status_detail = "direct sample had no telemetry";
                } else if (!runtime_home_published &&
                           snapshot_mirror_configured &&
                           !snapshot_mirror_published) {
                    status.status_detail =
                        "direct sample could not publish runtime home or snapshot mirror";
                } else if (!runtime_home_published) {
                    status.status_detail =
                        "direct sample could not publish runtime home";
                } else {
                    status.status_detail =
                        "direct sample could not publish snapshot mirror";
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const auto since_success_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_success_time).count();
            status.stale = static_cast<std::uint64_t>(since_success_ms) >
                           static_cast<std::uint64_t>(staleness_threshold_ms);

            if (csv_logger.is_open()) {
                csv_logger.WriteRow(BuildReadLoopCsvRow(
                    runtime_snapshot,
                    RuntimeReadLoopLogState{
                        .telemetry_available = telemetry_available,
                        .runtime_home_published = runtime_home_published,
                        .snapshot_mirror_configured = snapshot_mirror_configured,
                        .snapshot_mirror_published = snapshot_mirror_published,
                        .successful_polls = status.successful_polls,
                        .skipped_polls = status.skipped_polls,
                        .stale = status.stale,
                        .status_detail = status.status_detail,
                    }));
            }

            if (!telemetry_available || !wrote_outputs) {
                AppendRuntimeEvent(
                    impl_->runtime_home,
                    RuntimeLogEvent{
                        .mode = "read-loop",
                        .event_type = "read_loop.sample_skipped",
                        .detail = status.status_detail,
                        .success = false,
                        .snapshot_time_iso = runtime_snapshot.snapshot_time_iso,
                        .log_csv_path = status.log_csv_path,
                        .event_log_path = status.event_log_path,
                        .amd_sensor_count = static_cast<std::uint32_t>(
                            runtime_snapshot.amd_sensors.size()),
                        .fan_count = static_cast<std::uint32_t>(
                            runtime_snapshot.fans.size()),
                        .gpu_available = runtime_snapshot.gpu.available,
                        .successful_polls = status.successful_polls,
                        .skipped_polls = status.skipped_polls,
                        .stale = status.stale,
                        .telemetry_available = telemetry_available,
                        .runtime_home_published = runtime_home_published,
                        .snapshot_mirror_configured =
                            snapshot_mirror_configured,
                        .snapshot_mirror_published =
                            snapshot_mirror_published,
                    });
            }
        } catch (const std::exception& error) {
            ++status.skipped_polls;
            status.status_detail =
                std::string("direct sample failed: ") + error.what();
            AppendRuntimeEvent(
                impl_->runtime_home,
                RuntimeLogEvent{
                    .mode = "read-loop",
                    .event_type = "read_loop.sample_failed",
                    .detail = status.status_detail,
                    .success = false,
                    .log_csv_path = status.log_csv_path,
                    .event_log_path = status.event_log_path,
                    .successful_polls = status.successful_polls,
                    .skipped_polls = status.skipped_polls,
                });
        }

        const auto now = std::chrono::steady_clock::now();
        const auto since_success_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_success_time).count();
        status.stale = static_cast<std::uint64_t>(since_success_ms) >
                       static_cast<std::uint64_t>(staleness_threshold_ms);
        WriteRuntimeStatusFile(impl_->runtime_home, status);

        std::unique_lock<std::mutex> lock(impl_->wake_mutex);
        impl_->wake_cv.wait_for(
            lock,
            std::chrono::milliseconds(poll_ms),
            [this] {
                return impl_->stop_requested.load() ||
                       RuntimeStopRequested(impl_->runtime_home);
            });
    }

    publish_status("shutdown", "stop requested");
    AppendRuntimeEvent(
        impl_->runtime_home,
        RuntimeLogEvent{
            .mode = "read-loop",
            .event_type = "read_loop.shutdown",
            .detail = "stop requested",
            .success = true,
            .log_csv_path = status.log_csv_path,
            .event_log_path = status.event_log_path,
            .successful_polls = status.successful_polls,
            .skipped_polls = status.skipped_polls,
            .stale = status.stale,
            .snapshot_mirror_configured =
                !impl_->config.snapshot_path.empty(),
        });
    return 0;
}

}  // namespace svg_mb_control
