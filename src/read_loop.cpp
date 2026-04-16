#include "read_loop.h"

#include "amd_reader.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
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
#include <fstream>
#include <mutex>
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

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2u);
    for (char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::array<char, 8> escape{};
                    std::snprintf(escape.data(), escape.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += escape.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

bool WriteRuntimeStatusFile(const std::filesystem::path& runtime_home,
                            const ReadLoop::Status& status) {
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path target = runtime_home / "control_runtime.json";
    const std::filesystem::path temp = runtime_home / "control_runtime.json.tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return false;
        }
        stream << "{\n"
               << "  \"schema_version\": 1,\n"
               << "  \"status\": \"" << JsonEscape(status.status) << "\",\n"
               << "  \"status_detail\": \"" << JsonEscape(status.status_detail) << "\",\n"
               << "  \"last_refresh\": \"" << JsonEscape(status.last_refresh_iso) << "\",\n"
               << "  \"snapshot_source\": \"" << JsonEscape(status.snapshot_source) << "\",\n"
               << "  \"restart_count\": " << status.restart_count << ",\n"
               << "  \"skipped_polls\": " << status.skipped_polls << ",\n"
               << "  \"successful_polls\": " << status.successful_polls << ",\n"
               << "  \"stale\": " << (status.stale ? "true" : "false") << ",\n"
               << "  \"child_pid\": " << status.child_pid << "\n"
               << "}\n";
        stream.flush();
        if (stream.fail()) {
            return false;
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
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

    Status status;
    status.status = "running";
    status.status_detail = "starting";
    status.snapshot_source = "direct-runtime-snapshot";

    auto publish_status = [&](const std::string& state,
                              const std::string& detail) {
        status.status = state;
        status.status_detail = detail;
        WriteRuntimeStatusFile(impl_->runtime_home, status);
    };

    publish_status("running", "initializing direct readers");

    const RuntimeWritePolicy runtime_policy =
        ResolveRuntimeWritePolicy(&impl_->config);
    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(runtime_policy);
    } catch (const std::exception& error) {
        publish_status("direct-read-failed",
                       std::string("direct reader init failed: ") +
                           error.what());
        return 1;
    }

    AmdReader amd_reader;
    GpuReader gpu_reader;
    auto last_success_time = std::chrono::steady_clock::now();

    while (!impl_->stop_requested.load()) {
        try {
            RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
                amd_reader, gpu_reader, *fan_writer, runtime_policy);

            bool wrote_outputs = WriteRuntimeSnapshotFile(
                impl_->runtime_home, runtime_snapshot);
            if (!impl_->config.snapshot_path.empty()) {
                wrote_outputs = WriteRuntimeSnapshotJsonFile(
                                    impl_->config.snapshot_path,
                                    runtime_snapshot) &&
                                wrote_outputs;
            }

            if (wrote_outputs &&
                RuntimeSnapshotHasTelemetry(runtime_snapshot)) {
                last_success_time = std::chrono::steady_clock::now();
                ++status.successful_polls;
                status.stale = false;
                status.last_refresh_iso = FormatLocalIso8601(
                    std::chrono::system_clock::now());
                status.status_detail = "direct sample refreshed";
            } else {
                ++status.skipped_polls;
                status.status_detail = wrote_outputs
                    ? "direct sample had no telemetry"
                    : "direct sample could not be published";
            }
        } catch (const std::exception& error) {
            ++status.skipped_polls;
            status.status_detail =
                std::string("direct sample failed: ") + error.what();
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
            [this] { return impl_->stop_requested.load(); });
    }

    publish_status("shutdown", "stop requested");
    return 0;
}

}  // namespace svg_mb_control
