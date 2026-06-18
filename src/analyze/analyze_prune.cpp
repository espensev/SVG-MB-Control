#include "analyze_prune.h"

#include "analyze_db.h"
#include "analyze_json_artifacts.h"
#include "runtime_util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace svg_mb_control::analyze {

namespace {

struct ArchiveBundle {
    std::filesystem::path manifest_path;
    std::filesystem::path csv_path;
    ManifestData manifest;
    std::uintmax_t bytes = 0u;
};

struct DbRunCandidate {
    std::int64_t id = 0;
    std::string session_start;
    std::string mode;
};

std::filesystem::path CanonicalizeForLookup(
    const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec).lexically_normal();
}

bool HasManifestJsonSuffix(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    constexpr std::string_view suffix = ".manifest.json";
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

std::filesystem::path ResolvePairedCsv(
    const std::filesystem::path& manifest_path,
    const ManifestData& manifest) {
    std::error_code ec;
    if (manifest.csv_archive_path.has_value()) {
        const std::filesystem::path configured = *manifest.csv_archive_path;
        if (std::filesystem::exists(configured, ec)) {
            return configured;
        }
        ec.clear();
        if (configured.is_relative()) {
            const auto adjacent = manifest_path.parent_path() / configured;
            if (std::filesystem::exists(adjacent, ec)) {
                return adjacent;
            }
            ec.clear();
        }
    }

    std::filesystem::path adjacent = manifest_path;
    adjacent.replace_extension();
    if (adjacent.extension() == ".manifest") {
        adjacent.replace_extension(".csv");
        if (std::filesystem::exists(adjacent, ec)) {
            return adjacent;
        }
        ec.clear();
    }

    const std::string name = manifest_path.filename().string();
    constexpr std::string_view suffix = ".manifest.json";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
            0) {
        const std::string csv_name =
            name.substr(0, name.size() - suffix.size()) + ".csv";
        const auto csv_path = manifest_path.parent_path() / csv_name;
        if (std::filesystem::exists(csv_path, ec)) {
            return csv_path;
        }
    }
    return {};
}

std::uintmax_t FileSizeIfPresent(const std::filesystem::path& path) {
    if (path.empty()) {
        return 0u;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return 0u;
    }
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0u : size;
}

std::uintmax_t DatabaseDiskBytes(const std::filesystem::path& db_path) {
    std::uintmax_t bytes = FileSizeIfPresent(db_path);
    bytes += FileSizeIfPresent(std::filesystem::path(db_path.string() + "-wal"));
    bytes += FileSizeIfPresent(std::filesystem::path(db_path.string() + "-shm"));
    return bytes;
}

std::uintmax_t BundleSize(const std::filesystem::path& manifest_path,
                          const std::filesystem::path& csv_path) {
    return FileSizeIfPresent(manifest_path) + FileSizeIfPresent(csv_path);
}

std::optional<std::filesystem::file_time_type> BundleWriteTime(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& csv_path) {
    std::error_code ec;
    const std::filesystem::path primary =
        !csv_path.empty() ? csv_path : manifest_path;
    auto mtime = std::filesystem::last_write_time(primary, ec);
    if (!ec) {
        return mtime;
    }
    ec.clear();
    mtime = std::filesystem::last_write_time(manifest_path, ec);
    if (!ec) {
        return mtime;
    }
    return std::nullopt;
}

bool IsOlderThanRetention(const ArchiveBundle& bundle,
                          std::uint32_t retain_days) {
    if (retain_days == 0u) {
        return false;
    }
    const auto mtime =
        BundleWriteTime(bundle.manifest_path, bundle.csv_path);
    if (!mtime.has_value()) {
        return false;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(
                            24ll * static_cast<long long>(retain_days));
    return *mtime < cutoff;
}

std::vector<std::filesystem::path> CollectArchiveManifestPaths(
    const std::filesystem::path& runtime_home) {
    std::vector<std::filesystem::path> out;
    const auto archive_dir = runtime_home / "logs" / "archive";
    std::error_code ec;
    if (!std::filesystem::is_directory(archive_dir, ec) || ec) {
        return out;
    }
    for (const auto& entry :
         std::filesystem::directory_iterator(archive_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (HasManifestJsonSuffix(entry.path())) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

class IngestIndex {
  public:
    explicit IngestIndex(std::filesystem::path db_path)
        : db_path_(std::move(db_path)) {}

    bool Open(std::string& error) {
        if (db_path_.empty()) {
            error = "database path is empty";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::exists(db_path_, ec) || ec) {
            error = "database does not exist: " + db_path_.string();
            return false;
        }
        try {
            db_.Open(db_path_);
            const int schema_version = GetSchemaVersion(db_);
            if (schema_version <= 0) {
                error = "database schema is missing";
                return false;
            }
        } catch (const std::exception& ex) {
            error = ex.what();
            return false;
        }
        opened_ = true;
        return true;
    }

    bool IsIngested(const std::filesystem::path& manifest_path,
                    const ManifestData& manifest) {
        if (!opened_) {
            return false;
        }

        const std::string canonical =
            CanonicalizeForLookup(manifest_path).string();
        Statement by_path = db_.Prepare(
            "SELECT 1 FROM runs WHERE manifest_path = ?1 LIMIT 1");
        by_path.BindText(1, canonical);
        if (by_path.Step()) {
            return true;
        }

        Statement by_session = db_.Prepare(
            "SELECT 1 FROM runs WHERE session_start = ?1 AND mode = ?2 "
            "LIMIT 1");
        by_session.BindText(1, manifest.session_start);
        by_session.BindText(2, manifest.mode);
        return by_session.Step();
    }

  private:
    std::filesystem::path db_path_;
    Database db_;
    bool opened_ = false;
};

bool RemoveFileIfPresent(const std::filesystem::path& path,
                         std::uintmax_t& bytes_deleted,
                         std::string& error) {
    if (path.empty()) {
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return true;
    }
    const auto size = FileSizeIfPresent(path);
    if (!std::filesystem::remove(path, ec) || ec) {
        error = "failed to remove " + path.string() + ": " + ec.message();
        return false;
    }
    bytes_deleted += size;
    return true;
}

bool DeleteBundle(const ArchiveBundle& bundle,
                  std::uintmax_t& bytes_deleted,
                  std::string& error) {
    for (const auto& path :
         std::array<std::filesystem::path, 2>{bundle.csv_path,
                                              bundle.manifest_path}) {
        if (!RemoveFileIfPresent(path, bytes_deleted, error)) {
            return false;
        }
    }
    return true;
}

std::string RetentionCutoffIso(std::uint32_t retain_days) {
    const auto cutoff = std::chrono::system_clock::now() -
                        std::chrono::hours(
                            24ll * static_cast<long long>(retain_days));
    return FormatLocalIso8601(cutoff);
}

std::vector<DbRunCandidate> SelectDbRunCandidates(Database& db,
                                                  std::uint32_t retain_days) {
    std::vector<DbRunCandidate> out;
    const std::string cutoff = RetentionCutoffIso(retain_days);
    Statement stmt = db.Prepare(
        "SELECT id, session_start, mode FROM runs "
        "WHERE session_start < ?1 ORDER BY session_start, id");
    stmt.BindText(1, cutoff);
    while (stmt.Step()) {
        out.push_back({
            .id = stmt.ColumnInt(0),
            .session_start = stmt.ColumnText(1),
            .mode = stmt.ColumnText(2),
        });
    }
    return out;
}

std::int64_t QueryCount(Database& db, std::string_view sql) {
    Statement stmt = db.Prepare(sql);
    if (!stmt.Step()) {
        return 0;
    }
    return stmt.ColumnInt(0);
}

int CountOrphanRows(Database& db) {
    const std::int64_t tick_orphans = QueryCount(db, R"sql(
SELECT COUNT(*) FROM tick_samples AS ts
WHERE NOT EXISTS (SELECT 1 FROM runs AS r WHERE r.id = ts.run_id)
)sql");
    const std::int64_t fan_orphans = QueryCount(db, R"sql(
SELECT COUNT(*) FROM tick_fan_samples AS fs
WHERE NOT EXISTS (
    SELECT 1 FROM tick_samples AS ts
    WHERE ts.run_id = fs.run_id AND ts.tick_count = fs.tick_count)
)sql");
    const std::int64_t channel_orphans = QueryCount(db, R"sql(
SELECT COUNT(*) FROM tick_channel_samples AS cs
WHERE NOT EXISTS (
    SELECT 1 FROM tick_samples AS ts
    WHERE ts.run_id = cs.run_id AND ts.tick_count = cs.tick_count)
)sql");
    const std::int64_t event_orphans = QueryCount(db, R"sql(
SELECT COUNT(*) FROM events AS e
WHERE e.run_id IS NOT NULL
  AND NOT EXISTS (SELECT 1 FROM runs AS r WHERE r.id = e.run_id)
)sql");
    return static_cast<int>(tick_orphans + fan_orphans + channel_orphans +
                            event_orphans);
}

bool PruneDbRuns(const std::filesystem::path& db_path,
                 const PruneOptions& options,
                 PruneSummary& summary) {
    if (!options.db_retain_days.has_value()) {
        return true;
    }
    if (*options.db_retain_days == 0u) {
        if (!options.quiet) {
            std::cout << "db prune disabled: db_retain_days=0\n";
        }
        return true;
    }

    try {
        Database db;
        db.Open(db_path);
        const int schema_version = GetSchemaVersion(db);
        if (schema_version <= 0) {
            std::cerr << "Error: database schema is missing: "
                      << db_path.string() << '\n';
            return false;
        }

        summary.db_bytes_before = DatabaseDiskBytes(db_path);
        const std::vector<DbRunCandidate> candidates =
            SelectDbRunCandidates(db, *options.db_retain_days);
        summary.db_candidates = static_cast<int>(candidates.size());

        if (!options.quiet) {
            for (const auto& candidate : candidates) {
                std::cout << "db prune "
                          << (options.apply ? "candidate" : "would_delete")
                          << " run_id=" << candidate.id
                          << " session_start=" << candidate.session_start
                          << " mode=" << candidate.mode << '\n';
            }
        }

        if (!options.apply || candidates.empty()) {
            summary.db_bytes_after = summary.db_bytes_before;
            return true;
        }

        {
            Transaction txn(db.handle());
            Statement del = db.Prepare("DELETE FROM runs WHERE id = ?1");
            for (const auto& candidate : candidates) {
                del.BindInt(1, candidate.id);
                del.Step();
                del.Reset();
                ++summary.db_deleted_runs;
            }
            txn.Commit();
        }

        summary.db_orphan_rows = CountOrphanRows(db);
        if (summary.db_orphan_rows != 0) {
            std::cerr << "Error: DB prune left orphan rows: "
                      << summary.db_orphan_rows << '\n';
            return false;
        }

        if (summary.db_deleted_runs > 0) {
            db.Exec("VACUUM");
            db.Exec("PRAGMA wal_checkpoint(TRUNCATE)");
            summary.db_reclaim_ran = true;
        }
        summary.db_bytes_after = DatabaseDiskBytes(db_path);
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "Error: DB prune failed: " << ex.what() << '\n';
        return false;
    }
}

void PrintBundleDecision(const char* action,
                         const ArchiveBundle& bundle,
                         const char* reason) {
    std::cout << "prune " << action
              << " manifest=" << bundle.manifest_path.string();
    if (!bundle.csv_path.empty()) {
        std::cout << " csv=" << bundle.csv_path.string();
    }
    std::cout << " status=" << bundle.manifest.status
              << " bytes=" << bundle.bytes
              << " reason=" << reason << '\n';
}

}  // namespace

int RunAnalyzePrune(const PruneOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.runtime_home, ec)) {
        std::cerr << "Error: runtime_home is not a directory: "
                  << options.runtime_home.string() << '\n';
        return 1;
    }

    const std::filesystem::path db_path = options.db_path.empty()
        ? options.runtime_home / "svg_mb_control.db"
        : options.db_path;

    std::string db_error;
    IngestIndex ingest_index(db_path);
    const bool db_ready = ingest_index.Open(db_error);
    if (!db_ready && options.db_retain_days.has_value() &&
        *options.db_retain_days > 0u) {
        std::cerr << "Error: DB retention requested but " << db_error << '\n';
        return 1;
    }
    if (!db_ready && !options.quiet) {
        std::cerr << "Warning: prune will not delete raw bundles because "
                  << db_error << '\n';
    }

    PruneSummary summary;
    const auto manifest_paths = CollectArchiveManifestPaths(options.runtime_home);
    summary.archive_manifests_scanned =
        static_cast<int>(manifest_paths.size());

    for (const auto& manifest_path : manifest_paths) {
        ManifestData manifest;
        try {
            manifest = ParseRuntimeManifest(manifest_path);
        } catch (const std::exception& ex) {
            ++summary.skipped_errors;
            if (!options.quiet) {
                std::cerr << "Warning: skipping manifest "
                          << manifest_path.string() << ": " << ex.what()
                          << '\n';
            }
            continue;
        }

        ArchiveBundle bundle{
            .manifest_path = manifest_path,
            .csv_path = ResolvePairedCsv(manifest_path, manifest),
            .manifest = std::move(manifest),
        };
        bundle.bytes = BundleSize(bundle.manifest_path, bundle.csv_path);

        if (bundle.manifest.status == "running") {
            ++summary.skipped_running;
            if (!options.quiet) {
                PrintBundleDecision("skip", bundle, "running");
            }
            continue;
        }
        if (!IsOlderThanRetention(bundle, options.retain_days)) {
            ++summary.skipped_recent;
            if (!options.quiet) {
                PrintBundleDecision("skip", bundle, "within_retention");
            }
            continue;
        }
        if (!db_ready || !ingest_index.IsIngested(bundle.manifest_path,
                                                  bundle.manifest)) {
            ++summary.skipped_not_ingested;
            if (!options.quiet) {
                PrintBundleDecision("skip", bundle, "not_ingested");
            }
            continue;
        }

        ++summary.candidates;
        summary.bytes_selected += bundle.bytes;
        if (!options.apply) {
            if (!options.quiet) {
                PrintBundleDecision("would_delete", bundle, "eligible");
            }
            continue;
        }

        std::uintmax_t bytes_deleted = 0u;
        std::string delete_error;
        if (!DeleteBundle(bundle, bytes_deleted, delete_error)) {
            ++summary.skipped_errors;
            std::cerr << "Warning: " << delete_error << '\n';
            continue;
        }
        ++summary.deleted_bundles;
        summary.bytes_deleted += bytes_deleted;
        if (!options.quiet) {
            PrintBundleDecision("deleted", bundle, "eligible");
        }
    }

    if (!PruneDbRuns(db_path, options, summary)) {
        ++summary.skipped_errors;
    }

    std::cout << "analyze prune: runtime_home=" << options.runtime_home.string()
              << " db=" << db_path.string()
              << " dry_run=" << (options.apply ? "false" : "true")
              << " retain_days=" << options.retain_days
              << " db_retain_days="
              << (options.db_retain_days.has_value()
                      ? std::to_string(*options.db_retain_days)
                      : std::string("disabled"))
              << " scanned=" << summary.archive_manifests_scanned
              << " candidates=" << summary.candidates
              << " deleted=" << summary.deleted_bundles
              << " skipped_running=" << summary.skipped_running
              << " skipped_recent=" << summary.skipped_recent
              << " skipped_not_ingested=" << summary.skipped_not_ingested
              << " skipped_errors=" << summary.skipped_errors
              << " bytes_selected=" << summary.bytes_selected
              << " bytes_deleted=" << summary.bytes_deleted
              << " db_candidates=" << summary.db_candidates
              << " db_deleted_runs=" << summary.db_deleted_runs
              << " db_orphan_rows=" << summary.db_orphan_rows
              << " db_reclaim_ran="
              << (summary.db_reclaim_ran ? "true" : "false")
              << " db_bytes_before=" << summary.db_bytes_before
              << " db_bytes_after=" << summary.db_bytes_after << '\n';
    return summary.skipped_errors == 0 ? 0 : 1;
}

}  // namespace svg_mb_control::analyze
