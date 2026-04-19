#pragma once

#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svg_mb_control {

constexpr std::size_t kRuntimeLogFanChannelCount = 7u;

struct RuntimeControlChannelLogState {
    std::uint32_t channel = 0u;
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double setpoint_pct = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t total_writes = 0u;
    bool write_active = false;
    bool baseline_captured = false;
};

struct RuntimeReadLoopLogState {
    bool telemetry_available = false;
    bool runtime_home_published = false;
    bool snapshot_mirror_configured = false;
    bool snapshot_mirror_published = false;
    std::uint64_t successful_polls = 0u;
    std::uint64_t skipped_polls = 0u;
    bool stale = true;
    std::string status_detail;
};

struct RuntimeLogEvent {
    std::string event_time_iso;
    std::string mode;
    std::string event_type;
    std::string detail;
    std::optional<std::uint32_t> channel;
    std::optional<std::uint64_t> tick_count;
    std::optional<double> observed_temp_c;
    std::optional<double> setpoint_pct;
    std::optional<double> target_pct;
    std::optional<bool> success;
    std::string snapshot_time_iso;
    std::string log_csv_path;
    std::string event_log_path;
    std::optional<std::uint32_t> amd_sensor_count;
    std::optional<std::uint32_t> fan_count;
    std::optional<bool> gpu_available;
    std::optional<std::uint64_t> successful_polls;
    std::optional<std::uint64_t> skipped_polls;
    std::optional<bool> stale;
    std::optional<bool> telemetry_available;
    std::optional<bool> runtime_home_published;
    std::optional<bool> snapshot_mirror_configured;
    std::optional<bool> snapshot_mirror_published;
};

std::filesystem::path ResolveRuntimeLogsDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeArchiveDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home);

class RuntimeCsvLogger {
  public:
    RuntimeCsvLogger(std::filesystem::path runtime_home,
                     std::uint32_t rotate_hours,
                     std::uint32_t retain_days);
    ~RuntimeCsvLogger();

    RuntimeCsvLogger(const RuntimeCsvLogger&) = delete;
    RuntimeCsvLogger& operator=(const RuntimeCsvLogger&) = delete;

    bool Open(std::string_view mode, std::string_view header_line);
    bool MaybeRotate();
    bool WriteRow(std::string_view row);
    void Close();

    bool is_open() const;
    const std::filesystem::path& active_archive_path() const;
    const std::filesystem::path& mirror_path() const;

  private:
    bool OpenNewChunk();
    void WritePrologue();
    void PruneOldArchives();

    std::filesystem::path runtime_home_;
    std::filesystem::path logs_dir_;
    std::filesystem::path archive_dir_;
    std::filesystem::path active_archive_path_;
    std::filesystem::path mirror_path_;
    std::uint32_t rotate_hours_ = 0u;
    std::uint32_t retain_days_ = 0u;
    std::chrono::system_clock::time_point opened_at_{};
    std::string mode_;
    std::string header_line_;
    std::ofstream archive_stream_;
    std::ofstream mirror_stream_;
};

std::string BuildReadLoopCsvHeader();
std::string BuildReadLoopCsvRow(const RuntimeSnapshot& snapshot,
                                const RuntimeReadLoopLogState& state);

std::string BuildControlLoopCsvHeader();
std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    std::uint64_t tick_count,
    const std::vector<RuntimeControlChannelLogState>& channels);

bool AppendRuntimeEvent(const std::filesystem::path& runtime_home,
                        const RuntimeLogEvent& event);

}  // namespace svg_mb_control
