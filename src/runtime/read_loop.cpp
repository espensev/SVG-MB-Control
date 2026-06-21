#include "read_loop.h"

#include "amd_reader.h"
#include "control_scheduler.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "runtime_artifacts.h"
#include "runtime_csv_rows.h"
#include "runtime_lifecycle.h"
#include "runtime_snapshot.h"
#include "runtime_status.h"
#include "runtime_write_policy.h"

#include "windows_lean.h"

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

std::uint32_t ResolveStalenessThresholdMs(const ControlConfig& config) {
    if (config.staleness_threshold_ms > 0u) {
        return config.staleness_threshold_ms;
    }
    const std::uint32_t poll = config.poll_ms > 0u ? config.poll_ms : 1000u;
    return poll * 3u;
}

// Outcome of one read-loop poll publish step: which sinks the freshly sampled
// snapshot reached. wrote_outputs collapses the "runtime home + (no mirror or
// mirror also wrote)" success guard so the parent can decide success/skipped
// from a single field.
struct ReadLoopSampleResult {
    bool telemetry_available = false;
    bool runtime_home_published = false;
    bool snapshot_mirror_configured = false;
    bool snapshot_mirror_published = false;
    bool wrote_outputs = false;
    std::string runtime_home_publish_error;
    std::string snapshot_mirror_publish_error;
};

ReadLoopSampleResult PublishSnapshot(
    const RuntimeSnapshot& runtime_snapshot,
    const std::filesystem::path& runtime_home,
    const std::filesystem::path& snapshot_mirror_path) {
    ReadLoopSampleResult result;
    result.telemetry_available = RuntimeSnapshotHasTelemetry(runtime_snapshot);
    result.runtime_home_published =
        WriteRuntimeSnapshotFile(runtime_home,
                                 runtime_snapshot,
                                 &result.runtime_home_publish_error);
    result.snapshot_mirror_configured = !snapshot_mirror_path.empty();
    if (result.snapshot_mirror_configured) {
        result.snapshot_mirror_published = WriteRuntimeSnapshotJsonFile(
            snapshot_mirror_path,
            runtime_snapshot,
            &result.snapshot_mirror_publish_error);
    }
    result.wrote_outputs =
        result.runtime_home_published &&
        (!result.snapshot_mirror_configured ||
         result.snapshot_mirror_published);
    return result;
}

// Failure-case detail string for read-loop polls. Caller still owns the
// success message because that branch also touches last_success_time and the
// successful_polls counter.
const char* DescribeReadLoopPollFailure(const ReadLoopSampleResult& result) {
    if (!result.telemetry_available) {
        return "direct sample had no telemetry";
    }
    if (!result.runtime_home_published &&
        result.snapshot_mirror_configured &&
        !result.snapshot_mirror_published) {
        return "direct sample could not publish runtime home or snapshot mirror";
    }
    if (!result.runtime_home_published) {
        return "direct sample could not publish runtime home";
    }
    return "direct sample could not publish snapshot mirror";
}

std::string BuildCsvWriteFailureDetail(const RuntimeCsvLogger& csv_logger) {
    std::ostringstream detail;
    detail << "runtime CSV row write failed";
    if (!csv_logger.last_error_sink().empty()) {
        detail << " sink=" << csv_logger.last_error_sink();
    }
    if (!csv_logger.last_error_detail().empty()) {
        detail << " detail=" << csv_logger.last_error_detail();
    }
    return detail.str();
}

void AppendCsvWriteFailureEvent(
    const std::filesystem::path& runtime_home,
    const RuntimeCsvLogger& csv_logger,
    const std::string& mode,
    const std::string& event_log_path,
    const RuntimeArtifactNaming& naming) {
    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = mode,
            .event_type = "runtime_logging.csv_write_failed",
            .detail = BuildCsvWriteFailureDetail(csv_logger),
            .success = false,
            .log_csv_path = csv_logger.active_archive_path().string(),
            .event_log_path = event_log_path,
        },
        naming);
}

void AppendCsvWriteRecoveredEvent(
    const std::filesystem::path& runtime_home,
    const RuntimeCsvLogger& csv_logger,
    const std::string& mode,
    const std::string& event_log_path,
    const RuntimeArtifactNaming& naming) {
    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = mode,
            .event_type = "runtime_logging.csv_write_recovered",
            .detail = "runtime CSV row writes recovered",
            .success = true,
            .log_csv_path = csv_logger.active_archive_path().string(),
            .event_log_path = event_log_path,
        },
        naming);
}

std::string BuildSnapshotPublishFailureDetail(
    const ReadLoopSampleResult& result,
    const std::filesystem::path& runtime_home,
    const std::filesystem::path& snapshot_mirror_path) {
    std::ostringstream detail;
    detail << "runtime snapshot publish failed";
    if (!result.runtime_home_published) {
        detail << " runtime_home_path="
               << (runtime_home / "current_state.json").string();
        if (!result.runtime_home_publish_error.empty()) {
            detail << " runtime_home_detail="
                   << result.runtime_home_publish_error;
        }
    }
    if (result.snapshot_mirror_configured &&
        !result.snapshot_mirror_published) {
        detail << " snapshot_mirror_path=" << snapshot_mirror_path.string();
        if (!result.snapshot_mirror_publish_error.empty()) {
            detail << " snapshot_mirror_detail="
                   << result.snapshot_mirror_publish_error;
        }
    }
    return detail.str();
}

void AppendSnapshotPublishFailureEvent(
    const std::filesystem::path& runtime_home,
    const ReadLoopSampleResult& result,
    const std::filesystem::path& snapshot_mirror_path,
    const std::string& event_log_path) {
    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = "read-loop",
            .event_type = "runtime_logging.snapshot_publish_failed",
            .detail = BuildSnapshotPublishFailureDetail(
                result, runtime_home, snapshot_mirror_path),
            .success = false,
            .event_log_path = event_log_path,
            .telemetry_available = result.telemetry_available,
            .runtime_home_published = result.runtime_home_published,
            .snapshot_mirror_configured =
                result.snapshot_mirror_configured,
            .snapshot_mirror_published =
                result.snapshot_mirror_published,
        });
}

void AppendSnapshotPublishRecoveredEvent(
    const std::filesystem::path& runtime_home,
    const std::filesystem::path& snapshot_mirror_path,
    const std::string& event_log_path) {
    std::string detail = "runtime snapshot publish recovered path=" +
        (runtime_home / "current_state.json").string();
    if (!snapshot_mirror_path.empty()) {
        detail += " snapshot_mirror_path=" + snapshot_mirror_path.string();
    }
    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = "read-loop",
            .event_type = "runtime_logging.snapshot_publish_recovered",
            .detail = detail,
            .success = true,
            .event_log_path = event_log_path,
        });
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
    // FEAT-0023 (REQ-MPROFILE-09): observational active-profile identity.
    status.active_profile_name = impl_->config.profile_name;
    status.active_profile_source = impl_->config.profile_resolution_source;
    status.event_log_path =
        ResolveRuntimeEventLogPath(impl_->runtime_home).string();
    ConfigureRuntimeEventLogRetention(
        ResolveRuntimeEventLogPath(impl_->runtime_home),
        RuntimeEventLogOptions{
            .rotate_hours = impl_->config.log_rotate_hours,
            .retain_days = impl_->config.log_retain_days,
        });

    auto publish_status = [&](const std::string& state,
                              const std::string& detail) {
        status.status = state;
        status.status_detail = detail;
        WriteReadLoopStatus(impl_->runtime_home, status);
    };

    RuntimeCsvLogger csv_logger(
        impl_->runtime_home,
        impl_->config.log_rotate_hours,
        impl_->config.log_retain_days,
        impl_->config.csv_flush_interval_rows,
        RuntimeArtifactNaming{},
        RuntimeCsvIdentity{
            .config_path = impl_->config.source_path,
            .runtime_policy_path =
                ResolveRuntimePolicySourcePath(&impl_->config),
        });
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

    // Reused across poll iterations; SampleDirectRuntimeSnapshot fully resets
    // it each call so no telemetry carries over (see direct_runtime_snapshot).
    RuntimeSnapshot runtime_snapshot;
    bool csv_write_failure_active = false;
    bool snapshot_publish_failure_active = false;

    while (!impl_->stop_requested.load() &&
           !RuntimeStopRequested(impl_->runtime_home) &&
           !RuntimeProfileCycleRequested(impl_->runtime_home)) {
        // FEAT-0023: a profile-cycle signal exits the read loop so the
        // supervisor can respawn the worker on the new profile. The read loop
        // writes no fans, so there is nothing to restore.
        try {
            SampleDirectRuntimeSnapshot(amd_reader, gpu_reader, *fan_writer,
                                        runtime_policy, runtime_snapshot);

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

            const ReadLoopSampleResult published = PublishSnapshot(
                runtime_snapshot, impl_->runtime_home,
                impl_->config.snapshot_path);
            if (published.telemetry_available &&
                !published.wrote_outputs &&
                !snapshot_publish_failure_active) {
                AppendSnapshotPublishFailureEvent(impl_->runtime_home,
                                                  published,
                                                  impl_->config.snapshot_path,
                                                  status.event_log_path);
                snapshot_publish_failure_active = true;
            } else if (published.wrote_outputs &&
                       snapshot_publish_failure_active) {
                AppendSnapshotPublishRecoveredEvent(
                    impl_->runtime_home,
                    impl_->config.snapshot_path,
                    status.event_log_path);
                snapshot_publish_failure_active = false;
            }

            if (published.wrote_outputs && published.telemetry_available) {
                last_success_time = std::chrono::steady_clock::now();
                ++status.successful_polls;
                status.stale = false;
                status.last_refresh_iso = FormatLocalIso8601(
                    std::chrono::system_clock::now());
                status.status_detail = "direct sample refreshed";
            } else {
                ++status.skipped_polls;
                status.status_detail = DescribeReadLoopPollFailure(published);
            }

            const auto now = std::chrono::steady_clock::now();
            const auto since_success_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_success_time).count();
            status.stale = static_cast<std::uint64_t>(since_success_ms) >
                           static_cast<std::uint64_t>(staleness_threshold_ms);

            if (csv_logger.is_open()) {
                const bool row_written = csv_logger.WriteRow(BuildReadLoopCsvRow(
                    runtime_snapshot,
                    RuntimeReadLoopLogState{
                        .telemetry_available = published.telemetry_available,
                        .runtime_home_published =
                            published.runtime_home_published,
                        .snapshot_mirror_configured =
                            published.snapshot_mirror_configured,
                        .snapshot_mirror_published =
                            published.snapshot_mirror_published,
                        .successful_polls = status.successful_polls,
                        .skipped_polls = status.skipped_polls,
                        .stale = status.stale,
                        .status_detail = status.status_detail,
                    }));
                if (!row_written && !csv_write_failure_active) {
                    AppendCsvWriteFailureEvent(impl_->runtime_home,
                                               csv_logger,
                                               "read-loop",
                                               status.event_log_path,
                                               RuntimeArtifactNaming{});
                    csv_write_failure_active = true;
                } else if (row_written && csv_write_failure_active) {
                    AppendCsvWriteRecoveredEvent(impl_->runtime_home,
                                                 csv_logger,
                                                 "read-loop",
                                                 status.event_log_path,
                                                 RuntimeArtifactNaming{});
                    csv_write_failure_active = false;
                }
            }

            if (!published.telemetry_available || !published.wrote_outputs) {
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
                        .telemetry_available = published.telemetry_available,
                        .runtime_home_published =
                            published.runtime_home_published,
                        .snapshot_mirror_configured =
                            published.snapshot_mirror_configured,
                        .snapshot_mirror_published =
                            published.snapshot_mirror_published,
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
        WriteReadLoopStatus(impl_->runtime_home, status);

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
