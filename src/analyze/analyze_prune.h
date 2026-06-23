#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace svg_mb_control::analyze {

struct PruneOptions {
    std::filesystem::path runtime_home;
    std::filesystem::path db_path;
    std::uint32_t retain_days = 7u;
    std::optional<std::uint32_t> db_retain_days;
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
    int db_candidates = 0;
    int db_deleted_runs = 0;
    int db_orphan_rows = 0;
    std::uintmax_t bytes_selected = 0u;
    std::uintmax_t bytes_deleted = 0u;
    std::uintmax_t db_bytes_before = 0u;
    std::uintmax_t db_bytes_after = 0u;
    bool db_reclaim_ran = false;
};

int RunAnalyzePrune(const PruneOptions& options);

}  // namespace svg_mb_control::analyze
