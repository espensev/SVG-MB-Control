#pragma once

#include <cstdint>
#include <filesystem>

namespace svg_mb_control::analyze {

struct PruneOptions {
    std::filesystem::path runtime_home;
    std::filesystem::path db_path;
    std::uint32_t retain_days = 7u;
    bool apply = false;
    bool quiet = false;
};

struct PruneSummary {
    int archive_manifests_scanned = 0;
    int candidates = 0;
    int deleted_bundles = 0;
    int skipped_running = 0;
    int skipped_recent = 0;
    int skipped_not_ingested = 0;
    int skipped_errors = 0;
    std::uintmax_t bytes_selected = 0u;
    std::uintmax_t bytes_deleted = 0u;
};

int RunAnalyzePrune(const PruneOptions& options);

}  // namespace svg_mb_control::analyze
