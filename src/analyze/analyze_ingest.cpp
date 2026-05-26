#include "analyze_ingest.h"

#include "analyze_csv.h"
#include "analyze_db.h"
#include "analyze_ingest_db.h"
#include "analyze_json_artifacts.h"
#include "runtime_util.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace svg_mb_control::analyze {

namespace {

using svg_mb_control::FormatLocalIso8601;

std::filesystem::path Canonicalize(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return std::filesystem::absolute(path).lexically_normal();
    }
    return canonical;
}

std::filesystem::path ResolvePairedCsv(
    const std::filesystem::path& manifest_path,
    const ManifestData& manifest) {
    if (manifest.csv_archive_path.has_value()) {
        std::error_code ec;
        if (std::filesystem::exists(*manifest.csv_archive_path, ec)) {
            return *manifest.csv_archive_path;
        }
    }
    const std::filesystem::path parent = manifest_path.parent_path();
    const std::string manifest_name = manifest_path.filename().string();
    const std::string suffix_a = ".manifest.json";
    const std::string suffix_b = "_manifest.json";
    if (manifest_name.size() > suffix_a.size() &&
        manifest_name.compare(manifest_name.size() - suffix_a.size(),
                              suffix_a.size(), suffix_a) == 0) {
        const std::string stem = manifest_name.substr(
            0, manifest_name.size() - suffix_a.size());
        const auto candidate = parent / (stem + ".csv");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    if (manifest_name.size() > suffix_b.size() &&
        manifest_name.compare(manifest_name.size() - suffix_b.size(),
                              suffix_b.size(), suffix_b) == 0) {
        const std::string stem = manifest_name.substr(
            0, manifest_name.size() - suffix_b.size());
        const auto candidate = parent / (stem.substr(
            0, stem.find_last_of('_')) + "_output.csv");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    if (manifest_name == "svg_mb_control_manifest.json") {
        const auto candidate = parent / "svg_mb_control_output.csv";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::filesystem::path> CollectManifestPaths(
    const std::filesystem::path& runtime_home) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path logs = runtime_home / "logs";
    std::error_code ec;
    if (!std::filesystem::is_directory(logs, ec)) {
        return out;
    }
    const auto live = logs / "svg_mb_control_manifest.json";
    if (std::filesystem::exists(live, ec)) {
        out.push_back(live);
    }
    const auto archive = logs / "archive";
    if (std::filesystem::is_directory(archive, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(archive, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            const std::string suffix = ".manifest.json";
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(),
                             suffix.size(), suffix) == 0) {
                out.push_back(entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::filesystem::path> CollectPlantModelPaths(
    const std::filesystem::path& runtime_home) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const auto live = runtime_home / "plant_model.json";
    if (std::filesystem::exists(live, ec)) {
        out.push_back(live);
    }
    if (std::filesystem::is_directory(runtime_home, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(runtime_home, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name == "plant_model.json") {
                continue;
            }
            const std::string prefix = "plant_model_";
            const std::string suffix = ".json";
            if (name.size() > prefix.size() + suffix.size() &&
                name.compare(0, prefix.size(), prefix) == 0 &&
                name.compare(name.size() - suffix.size(),
                             suffix.size(), suffix) == 0) {
                out.push_back(entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int RunAnalyzeIngest(const IngestOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.runtime_home, ec)) {
        std::cerr << "Error: runtime_home is not a directory: "
                  << options.runtime_home.string() << '\n';
        return 1;
    }

    const std::filesystem::path db_path = options.db_path.empty()
        ? options.runtime_home / "svg_mb_control.db"
        : options.db_path;

    if (!db_path.parent_path().empty()) {
        std::filesystem::create_directories(db_path.parent_path(), ec);
    }

    Database db;
    try {
        db.Open(db_path);
    } catch (const std::exception& error) {
        std::cerr << "Error: failed to open database " << db_path.string()
                  << ": " << error.what() << '\n';
        return 1;
    }

    try {
        BootstrapSchema(db);
    } catch (const std::exception& error) {
        std::cerr << "Error: schema bootstrap failed: " << error.what() << '\n';
        return 1;
    }

    const int version = GetSchemaVersion(db);
    if (version != kSchemaVersion) {
        std::cerr << "Error: database schema version " << version
                  << " is not supported (this build expects "
                  << kSchemaVersion << ")\n";
        return 1;
    }

    const std::string ingested_at =
        FormatLocalIso8601(std::chrono::system_clock::now());

    IngestSummary summary;
    std::vector<RunWindow> run_windows;

    const auto manifests = CollectManifestPaths(options.runtime_home);
    for (const auto& manifest_path : manifests) {
        ManifestData manifest;
        try {
            manifest = ParseRuntimeManifest(manifest_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: skipping manifest "
                      << manifest_path.string() << ": " << error.what()
                      << '\n';
            continue;
        }
        const std::string canonical = Canonicalize(manifest_path).string();
        const bool manifest_seen = IsManifestPathInDb(db, canonical);
        const bool session_seen = IsSessionInDb(
            db, manifest.session_start, manifest.mode);
        if (manifest_seen || session_seen) {
            if (!options.force) {
                ++summary.runs_skipped;
                continue;
            }
            Transaction txn(db.handle());
            if (manifest_seen) {
                DeleteRunByManifestPath(db, canonical);
            }
            if (session_seen) {
                DeleteRunBySession(db, manifest.session_start, manifest.mode);
            }
            txn.Commit();
        }

        const std::filesystem::path csv_path =
            ResolvePairedCsv(manifest_path, manifest);
        ParsedCsv parsed_csv;
        if (!csv_path.empty()) {
            try {
                parsed_csv = ParseControlLoopCsv(csv_path);
            } catch (const std::exception& error) {
                std::cerr << "Warning: failed to parse CSV "
                          << csv_path.string() << ": " << error.what()
                          << '\n';
            }
        } else if (!options.quiet) {
            std::cerr << "Warning: no paired CSV found for "
                      << manifest_path.string() << '\n';
        }

        std::int64_t run_id = 0;
        try {
            Transaction txn(db.handle());
            run_id = InsertRun(db, canonical, csv_path, manifest, ingested_at);
            InsertTickRows(db, run_id, parsed_csv.rows);
            UpdateRunIngestCounts(
                db, run_id,
                static_cast<int>(parsed_csv.rows.size()), 0);
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert run for "
                      << manifest_path.string() << ": " << error.what()
                      << '\n';
            continue;
        }

        run_windows.push_back({run_id, manifest.session_start, manifest.mode});
        summary.tick_samples += static_cast<int>(parsed_csv.rows.size());
        ++summary.runs_ingested;

        if (!options.quiet) {
            std::cout << "ingested run id=" << run_id
                      << " session=" << manifest.session_start
                      << " ticks=" << parsed_csv.rows.size() << '\n';
        }
    }

    const std::filesystem::path events_path =
        options.runtime_home / "logs" / "svg_mb_control_events.jsonl";
    if (std::filesystem::exists(events_path, ec)) {
        std::vector<EventData> events;
        try {
            events = ParseEventsJsonl(events_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: failed to parse events: "
                      << error.what() << '\n';
        }

        if (options.force) {
            Transaction txn(db.handle());
            db.Exec("DELETE FROM events");
            txn.Commit();
        }

        std::vector<RunWindow> windows_for_attribution;
        if (options.force) {
            Statement query = db.Prepare(
                "SELECT id, session_start, mode FROM runs ORDER BY id");
            while (query.Step()) {
                windows_for_attribution.push_back({
                    query.ColumnInt(0),
                    query.ColumnText(1),
                    query.ColumnText(2),
                });
            }
        } else {
            windows_for_attribution = run_windows;
        }

        try {
            Transaction txn(db.handle());
            const int n = InsertEventsAttributed(
                db, events, windows_for_attribution);
            summary.events_ingested += n;

            for (const auto& w : windows_for_attribution) {
                Statement count = db.Prepare(
                    "SELECT COUNT(*) FROM events WHERE run_id = ?1");
                count.BindInt(1, w.run_id);
                count.Step();
                Statement update = db.Prepare(
                    "UPDATE runs SET event_count_ingested = ?1 WHERE id = ?2");
                update.BindInt(1, count.ColumnInt(0));
                update.BindInt(2, w.run_id);
                update.Step();
            }
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert events: " << error.what()
                      << '\n';
        }
    }

    const auto plant_paths = CollectPlantModelPaths(options.runtime_home);
    for (const auto& pm_path : plant_paths) {
        const std::string canonical = Canonicalize(pm_path).string();
        if (IsCapturePathInDb(db, canonical)) {
            if (!options.force) {
                ++summary.plant_models_skipped;
                continue;
            }
            Transaction txn(db.handle());
            DeleteCaptureByPath(db, canonical);
            txn.Commit();
        }

        PlantModelData data;
        try {
            data = ParsePlantModelCapture(pm_path);
        } catch (const std::exception& error) {
            std::cerr << "Warning: failed to parse plant model "
                      << pm_path.string() << ": " << error.what() << '\n';
            continue;
        }

        try {
            Transaction txn(db.handle());
            const std::int64_t capture_id =
                InsertPlantModelCapture(db, canonical, data, ingested_at);
            InsertPlantModelChannelsAndSteps(db, capture_id, data);
            txn.Commit();
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to insert plant model "
                      << pm_path.string() << ": " << error.what() << '\n';
            continue;
        }

        ++summary.plant_models_ingested;
        if (!options.quiet) {
            std::cout << "ingested plant_model " << pm_path.string()
                      << " channels=" << data.channels.size() << '\n';
        }
    }

    std::cout << "analyze ingest: db=" << db_path.string()
              << " runs_ingested=" << summary.runs_ingested
              << " runs_skipped=" << summary.runs_skipped
              << " tick_samples=" << summary.tick_samples
              << " events=" << summary.events_ingested
              << " plant_models=" << summary.plant_models_ingested
              << " plant_models_skipped=" << summary.plant_models_skipped
              << '\n';
    return 0;
}

}  // namespace svg_mb_control::analyze
