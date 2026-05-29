#pragma once

// Database write operations for analyze ingest: idempotency checks, deletes,
// and row inserts. Split out of analyze_ingest.cpp so the orchestration
// (path discovery, parse, summary) and the SQL row mapping are independent
// translation units. RunAnalyzeIngest stays in analyze_ingest.cpp.

#include "analyze_csv.h"
#include "analyze_db.h"
#include "analyze_json_artifacts.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svg_mb_control::analyze {

// Maps a run's session_start to its inserted row id, used to attribute
// events to the run that was active when they were emitted.
struct RunWindow {
    std::int64_t run_id;
    std::string session_start;
    std::string mode;
};

bool IsManifestPathInDb(Database& db, const std::string& canonical_path);
void DeleteRunByManifestPath(Database& db, const std::string& canonical_path);

bool IsSessionInDb(Database& db,
                   const std::string& session_start,
                   const std::string& mode);
void DeleteRunBySession(Database& db,
                        const std::string& session_start,
                        const std::string& mode);

bool IsCapturePathInDb(Database& db, const std::string& canonical_path);
void DeleteCaptureByPath(Database& db, const std::string& canonical_path);

std::int64_t InsertRun(Database& db,
                       const std::string& manifest_path_canonical,
                       const std::filesystem::path& csv_path,
                       const ManifestData& manifest,
                       const std::string& ingested_at);

void UpdateRunIngestCounts(Database& db,
                           std::int64_t run_id,
                           int row_count,
                           int event_count);

void InsertTickRows(Database& db,
                    std::int64_t run_id,
                    const std::vector<ParsedTickRow>& rows);

int InsertEventsAttributed(Database& db,
                           const std::vector<EventData>& events,
                           const std::vector<RunWindow>& runs);

// Attributes every event to one run id unconditionally (used by the
// manifest-free single-CSV ingest, which has no start-event session window).
int InsertEventsForRun(Database& db,
                       const std::vector<EventData>& events,
                       std::int64_t run_id);

std::int64_t InsertPlantModelCapture(Database& db,
                                     const std::string& path_canonical,
                                     const PlantModelData& data,
                                     const std::string& ingested_at);

void InsertPlantModelChannelsAndSteps(Database& db,
                                       std::int64_t capture_id,
                                       const PlantModelData& data);

}  // namespace svg_mb_control::analyze
