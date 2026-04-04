#include "read_loop.h"

#include "bench_bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

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

bool LooksLikeCompleteJsonObject(std::string_view text) {
    std::size_t start = 0u;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    if (start >= text.size() || text[start] != '{') {
        return false;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) {
        --end;
    }
    return end > start && text[end - 1u] == '}';
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2u);
    for (char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"':  output += "\\\""; break;
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

bool TryReadSnapshot(const std::filesystem::path& path,
                     std::string* parsed_text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    if (text.empty() || !LooksLikeCompleteJsonObject(text)) {
        return false;
    }
    *parsed_text = text;
    return true;
}

bool ReadSnapshotWithRetries(const std::filesystem::path& path,
                             std::uint32_t retry_count,
                             std::uint32_t backoff_ms,
                             std::string* parsed_text) {
    const std::uint32_t attempts = retry_count + 1u;
    for (std::uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        if (TryReadSnapshot(path, parsed_text)) {
            return true;
        }
        if (attempt + 1u < attempts) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(backoff_ms));
        }
    }
    return false;
}

std::filesystem::file_time_type SafeModificationTime(
    const std::filesystem::path& path) {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::filesystem::file_time_type::min();
    }
    return value;
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
    std::wstring bench_exe_path;

    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    std::atomic<bool> stop_requested{false};
};

ReadLoop::ReadLoop(ControlConfig config,
                   std::filesystem::path runtime_home,
                   std::wstring bench_exe_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->runtime_home = std::move(runtime_home);
    impl_->bench_exe_path = std::move(bench_exe_path);
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
    const std::uint32_t duration_ms = impl_->config.logger_service_duration_ms;
    const std::vector<std::wstring> child_args = {
        L"logger-service",
        L"--duration-ms",
        std::to_wstring(duration_ms),
    };

    std::error_code ec;
    std::filesystem::create_directories(impl_->runtime_home, ec);

    Status status;
    status.status = "running";
    status.status_detail = "starting";
    status.snapshot_source = impl_->config.snapshot_path.string();

    auto publish_status = [&](const std::string& state,
                              const std::string& detail) {
        status.status = state;
        status.status_detail = detail;
        WriteRuntimeStatusFile(impl_->runtime_home, status);
    };

    publish_status("running", "spawning child");

    std::unique_ptr<BenchChildSupervisor> supervisor =
        std::make_unique<BenchChildSupervisor>(impl_->bench_exe_path,
                                               child_args);
    try {
        supervisor->Start();
    } catch (const std::exception& error) {
        publish_status("child-died", std::string("initial start failed: ") +
                                         error.what());
        return 1;
    }

    std::filesystem::file_time_type last_mtime =
        std::filesystem::file_time_type::min();
    auto last_success_time = std::chrono::steady_clock::now();

    while (!impl_->stop_requested.load()) {
        if (!supervisor->IsRunning()) {
            const int exit_code = supervisor->LastExitCode();
            if (status.restart_count >= impl_->config.child_restart_budget) {
                publish_status("child-died",
                               std::string("restart budget exhausted after exit code ") +
                                   std::to_string(exit_code));
                return exit_code != 0 ? exit_code : 1;
            }
            ++status.restart_count;
            publish_status("running",
                           std::string("restarting child after exit code ") +
                               std::to_string(exit_code));

            {
                std::unique_lock<std::mutex> lock(impl_->wake_mutex);
                impl_->wake_cv.wait_for(
                    lock,
                    std::chrono::milliseconds(
                        impl_->config.child_restart_backoff_ms),
                    [this] { return impl_->stop_requested.load(); });
            }
            if (impl_->stop_requested.load()) {
                break;
            }

            supervisor = std::make_unique<BenchChildSupervisor>(
                impl_->bench_exe_path, child_args);
            try {
                supervisor->Start();
            } catch (const std::exception& error) {
                publish_status("child-died",
                               std::string("restart failed: ") + error.what());
                return 1;
            }
            status.child_pid = 0u;
            last_mtime = std::filesystem::file_time_type::min();
            continue;
        }

        const auto current_mtime = SafeModificationTime(
            impl_->config.snapshot_path);
        if (current_mtime != last_mtime &&
            current_mtime != std::filesystem::file_time_type::min()) {
            std::string snapshot_text;
            if (ReadSnapshotWithRetries(impl_->config.snapshot_path,
                                        impl_->config.snapshot_read_retry_count,
                                        impl_->config.snapshot_read_retry_backoff_ms,
                                        &snapshot_text)) {
                last_mtime = current_mtime;
                last_success_time = std::chrono::steady_clock::now();
                ++status.successful_polls;
                status.stale = false;
                status.last_refresh_iso = FormatLocalIso8601(
                    std::chrono::system_clock::now());
            } else {
                ++status.skipped_polls;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const auto since_success_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_success_time).count();
        status.stale = static_cast<std::uint64_t>(since_success_ms) >
                       static_cast<std::uint64_t>(staleness_threshold_ms);

        WriteRuntimeStatusFile(impl_->runtime_home, status);

        {
            std::unique_lock<std::mutex> lock(impl_->wake_mutex);
            impl_->wake_cv.wait_for(
                lock,
                std::chrono::milliseconds(poll_ms),
                [this] { return impl_->stop_requested.load(); });
        }
    }

    publish_status("shutdown", "stop requested");
    supervisor->RequestStop(2000u);
    publish_status("shutdown", "child stopped");
    return 0;
}

}  // namespace svg_mb_control
