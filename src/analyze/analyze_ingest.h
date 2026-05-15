#pragma once

#include <filesystem>

namespace svg_mb_control::analyze {

struct IngestOptions {
    std::filesystem::path runtime_home;
    std::filesystem::path db_path;  // empty -> runtime_home / svg_mb_control.db
    bool force = false;
    bool quiet = false;
};

struct IngestSummary {
    int runs_ingested = 0;
    int runs_skipped = 0;
    int tick_samples = 0;
    int events_ingested = 0;
    int plant_models_ingested = 0;
    int plant_models_skipped = 0;
};

int RunAnalyzeIngest(const IngestOptions& options);

}  // namespace svg_mb_control::analyze
