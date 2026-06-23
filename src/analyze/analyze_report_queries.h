#pragma once

#include "analyze_report.h"
#include "analyze_report_data.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::analyze {

class Database;

namespace report_detail {

struct TargetRun {
    std::int64_t id = 0;
    std::string session_start;
    std::string mode;
    std::string status;
    std::filesystem::path manifest_path;
    std::filesystem::path csv_archive_path;
    std::optional<std::string> tool_version;
    std::optional<std::string> git_hash;
    std::optional<std::int64_t> row_count_declared;
    std::optional<std::int64_t> row_count_ingested;
    std::optional<std::int64_t> event_count_declared;
    std::optional<std::int64_t> event_count_ingested;
    std::optional<std::string> csv_flush_policy;
    std::optional<std::string> mirror_mode;
    std::optional<std::string> last_update;
};

struct RobustnessCounts {
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;
};

struct ResponseDelay {
    std::optional<std::int64_t> onset_tick;
    std::optional<std::int64_t> response_tick;
    std::optional<double> response_delay_s;
};

std::optional<TargetRun> LookupTargetRun(
    Database& db,
    const ReportOptions& options,
    const std::filesystem::path& db_path);
RuntimeManifestEvidence LoadRuntimeManifestEvidence(
    const std::filesystem::path& manifest_path);

bool LoadTicks(Database& db, std::int64_t run_id,
               std::vector<TickRow>& out);
void ComputeElapsedTime(std::vector<TickRow>& ticks);
void AssignBands(std::vector<TickRow>& ticks, const ReportOptions& options);
bool LoadChannelStats(Database& db, std::int64_t run_id,
                      std::map<int, ChannelStats>& channels);
bool MergeFanStats(Database& db, std::int64_t run_id,
                   std::map<int, ChannelStats>& channels);
bool LoadRobustnessCounts(Database& db, std::int64_t run_id,
                          RobustnessCounts& out);
bool LoadEventFieldCounts(Database& db,
                          std::int64_t run_id,
                          const char* field_name,
                          const char* fallback,
                          std::map<std::string, int>& out);
std::map<int, double> ComputeIdleSetpointBaselines(
    Database& db, std::int64_t run_id,
    const std::vector<TickRow>& ticks);
ResponseDelay DetectResponseDelay(
    Database& db, std::int64_t run_id,
    const std::vector<TickRow>& ticks,
    const std::map<int, double>& idle_baselines,
    const ReportOptions& options);
TimingResourceStats SummariseTimingResources(
    const std::vector<TickRow>& ticks);
GpuResponseSummary SummariseGpuResponse(
    Database& db, std::int64_t run_id,
    const std::vector<TickRow>& ticks,
    const ReportOptions& options);
PackagePowerSummary SummarisePackagePower(Database& db, std::int64_t run_id);
CpuCyclesSummary SummariseCpuCycles(Database& db, std::int64_t run_id,
                                    std::optional<double> p0_mhz);
CpuCyclesSummary SummariseCpuCyclesAllcore(Database& db, std::int64_t run_id,
                                           std::optional<double> p0_mhz);
GpuPowerSummary SummariseGpuPower(Database& db, std::int64_t run_id);
GpuContextSummary SummariseGpuContext(Database& db, std::int64_t run_id);

}  // namespace report_detail
}  // namespace svg_mb_control::analyze
