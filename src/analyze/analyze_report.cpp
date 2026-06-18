#include "analyze_report.h"

#include "analyze_db.h"
#include "analyze_report_emit.h"
#include "analyze_report_queries.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <exception>
#include <utility>
#include <vector>

namespace svg_mb_control::analyze {

int RunAnalyzeReport(const ReportOptions& options) {
    using namespace report_detail;

    if (options.no_decision_record &&
        (options.decision_record_auto ||
         !options.decision_record_out_path.empty())) {
        std::cerr << "Error: --no-decision-record cannot be combined with "
                     "--decision-record-out.\n";
        return 1;
    }
    if (options.decision_record_auto && options.out_path.empty()) {
        std::cerr << "Error: --decision-record-out auto requires --out.\n";
        return 1;
    }

    const std::filesystem::path db_path = options.db_path.empty()
        ? options.runtime_home / "svg_mb_control.db"
        : options.db_path;

    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec) || ec) {
        std::cerr << "Error: database does not exist: " << db_path.string()
                  << '\n';
        return 1;
    }

    Database db;
    try {
        db.Open(db_path);
        if (GetSchemaVersion(db) <= 0) {
            std::cerr << "Error: database schema is missing.\n";
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: cannot open database: " << ex.what() << '\n';
        return 1;
    }

    std::optional<TargetRun> target;
    try {
        target = LookupTargetRun(db, options, db_path);
    } catch (const std::exception& ex) {
        std::cerr << "Error: run lookup failed: " << ex.what() << '\n';
        return 1;
    }
    if (!target.has_value()) {
        return 1;
    }
    const std::int64_t run_id = target->id;

    std::vector<TickRow> ticks;
    if (!LoadTicks(db, run_id, ticks)) {
        return 1;
    }
    if (ticks.empty()) {
        std::cerr << "Error: run " << run_id << " has no ingested ticks.\n";
        return 1;
    }
    ComputeElapsedTime(ticks);
    AssignBands(ticks, options);

    std::map<int, ChannelStats> channels;
    if (!LoadChannelStats(db, run_id, channels)) {
        return 1;
    }
    if (!MergeFanStats(db, run_id, channels)) {
        return 1;
    }

    RobustnessCounts robustness;
    if (!LoadRobustnessCounts(db, run_id, robustness)) {
        return 1;
    }
    std::map<std::string, int> event_severity_counts;
    if (!LoadEventFieldCounts(db, run_id, "severity", "unknown",
                              event_severity_counts)) {
        return 1;
    }
    std::map<std::string, int> event_error_code_counts;
    if (!LoadEventFieldCounts(db, run_id, "error_code", "none",
                              event_error_code_counts)) {
        return 1;
    }

    const auto idle_baselines =
        ComputeIdleSetpointBaselines(db, run_id, ticks);
    const ResponseDelay response =
        DetectResponseDelay(db, run_id, ticks, idle_baselines, options);

    ReportData data;
    data.run_id = run_id;
    data.session_start = target->session_start;
    data.mode = target->mode;
    data.status = target->status;
    data.manifest_path = target->manifest_path;
    data.csv_archive_path = target->csv_archive_path;
    data.tool_version = target->tool_version;
    data.git_hash = target->git_hash;
    data.row_count_declared = target->row_count_declared;
    data.row_count_ingested = target->row_count_ingested;
    data.event_count_declared = target->event_count_declared;
    data.event_count_ingested = target->event_count_ingested;
    data.csv_flush_policy = target->csv_flush_policy;
    data.mirror_mode = target->mirror_mode;
    data.last_update = target->last_update;
    data.manifest_evidence = LoadRuntimeManifestEvidence(data.manifest_path);
    data.ticks = std::move(ticks);
    data.idle = SummariseBand(data.ticks, Band::kIdle);
    data.load = SummariseBand(data.ticks, Band::kLoad);
    data.cool = SummariseBand(data.ticks, Band::kCooldown);
    data.channels = std::move(channels);
    data.onset_tick = response.onset_tick;
    data.response_tick = response.response_tick;
    data.response_delay_s = response.response_delay_s;
    data.timing_resources = SummariseTimingResources(data.ticks);
    data.gpu_response = SummariseGpuResponse(db, run_id, data.ticks, options);
    data.package_power = SummarisePackagePower(db, run_id);
    data.cpu_cycles = SummariseCpuCycles(db, run_id, options.p0_mhz);
    data.gpu_power = SummariseGpuPower(db, run_id);
    data.authority_reasserted = robustness.authority_reasserted;
    data.write_failures = robustness.write_failures;
    data.restore_failures = robustness.restore_failures;
    data.event_severity_counts = std::move(event_severity_counts);
    data.event_error_code_counts = std::move(event_error_code_counts);

    std::ostringstream report_stream;
    if (options.as_json) {
        EmitJsonReport(options, data, report_stream);
    } else {
        EmitTextReport(options, data, report_stream);
    }

    OutputArtifactPaths outputs;
    if (!options.out_path.empty()) {
        if (!WriteTextFile(options.out_path, report_stream.str(), "report")) {
            return 1;
        }
        outputs.report_path = options.out_path;
    } else {
        std::cout << report_stream.str();
    }

    std::filesystem::path decision_record_path;
    if (!options.no_decision_record) {
        if (!options.decision_record_out_path.empty()) {
            decision_record_path = options.decision_record_out_path;
        } else if (options.decision_record_auto ||
                   (!options.out_path.empty() && !options.as_json)) {
            decision_record_path = AutoDecisionRecordPath(options.out_path);
        }
    }
    if (!decision_record_path.empty()) {
        if (!WriteTextFile(decision_record_path,
                           BuildDecisionRecord(options, data),
                           "decision record")) {
            return 1;
        }
        outputs.decision_record_path = decision_record_path;
    }

    if (!options.manifest_out_path.empty() &&
        !WriteAnalysisManifest(options, data, outputs,
                               options.manifest_out_path)) {
        return 1;
    }
    return 0;
}

}  // namespace svg_mb_control::analyze
