#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace svg_mb_control {

struct RuntimeArtifactNaming {
    std::string archive_prefix = "svg_mb_control";
    std::string latest_csv_name = "svg_mb_control_output.csv";
    std::string latest_events_name = "svg_mb_control_events.jsonl";
    std::string latest_manifest_name = "svg_mb_control_manifest.json";
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

std::string FormatRuntimeLocalIso8601(
    std::chrono::system_clock::time_point tp);

std::filesystem::path ResolveRuntimeLogsDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeArchiveDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);
std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);
std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);

class RuntimeCsvLogger {
  public:
    RuntimeCsvLogger(std::filesystem::path runtime_home,
                     std::uint32_t rotate_hours,
                     std::uint32_t retain_days,
                     RuntimeArtifactNaming naming = RuntimeArtifactNaming{});
    ~RuntimeCsvLogger();

    RuntimeCsvLogger(const RuntimeCsvLogger&) = delete;
    RuntimeCsvLogger& operator=(const RuntimeCsvLogger&) = delete;

    bool Open(std::string_view mode, std::string_view header_line);
    bool MaybeRotate();
    bool WriteRow(std::string_view row);
    void Close();

    bool is_open() const;
    const std::filesystem::path& active_archive_path() const;
    const std::filesystem::path& active_manifest_path() const;
    const std::filesystem::path& mirror_path() const;
    const std::filesystem::path& manifest_path() const;
    std::uint64_t row_count() const;

  private:
    bool OpenNewChunk();
    void WritePrologue();
    void WriteManifest(std::string_view status);
    void CloseActiveChunk(std::string_view status);
    void PruneOldArchives();

    std::filesystem::path runtime_home_;
    std::filesystem::path logs_dir_;
    std::filesystem::path archive_dir_;
    std::filesystem::path active_archive_path_;
    std::filesystem::path active_manifest_path_;
    std::filesystem::path mirror_path_;
    std::filesystem::path manifest_path_;
    std::uint32_t rotate_hours_ = 0u;
    std::uint32_t retain_days_ = 0u;
    std::uint64_t row_count_ = 0u;
    std::chrono::system_clock::time_point opened_at_{};
    std::string mode_;
    std::string header_line_;
    RuntimeArtifactNaming naming_;
    std::ofstream archive_stream_;
    std::ofstream mirror_stream_;
};

bool AppendRuntimeEvent(const std::filesystem::path& runtime_home,
                        const RuntimeLogEvent& event,
                        const RuntimeArtifactNaming& naming =
                            RuntimeArtifactNaming{});

}  // namespace svg_mb_control
