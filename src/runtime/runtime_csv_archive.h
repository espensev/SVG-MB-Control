#pragma once

#include "runtime_paths.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace svg_mb_control {

class RuntimeCsvLogger {
  public:
    RuntimeCsvLogger(std::filesystem::path runtime_home,
                     std::uint32_t rotate_hours,
                     std::uint32_t retain_days,
                     std::uint32_t csv_flush_interval_rows = 1u,
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
    bool FlushStreams();
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
    std::uint32_t csv_flush_interval_rows_ = 1u;
    std::uint32_t rows_since_flush_ = 0u;
    std::uint64_t row_count_ = 0u;
    std::chrono::system_clock::time_point opened_at_{};
    std::string mode_;
    std::string header_line_;
    RuntimeArtifactNaming naming_;
    std::ofstream archive_stream_;
    std::ofstream mirror_stream_;
    std::string mirror_pending_rows_;
};

}  // namespace svg_mb_control
