#pragma once

#include "analyze_report.h"
#include "analyze_report_data.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace svg_mb_control::analyze::report_detail {

struct OutputArtifactPaths {
    std::filesystem::path report_path;
    std::filesystem::path decision_record_path;
};

std::vector<std::string> BuildDiagnosticFlags(
    const ReportOptions& options,
    const ReportData& data);
void EmitJsonReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out);
void EmitTextReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out);
std::string BuildDecisionRecord(const ReportOptions& options,
                                const ReportData& data);
std::filesystem::path AutoDecisionRecordPath(
    const std::filesystem::path& report_path);
bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& content,
                   const char* label);
bool WriteAnalysisManifest(const ReportOptions& options,
                           const ReportData& data,
                           const OutputArtifactPaths& outputs,
                           const std::filesystem::path& path);

}  // namespace svg_mb_control::analyze::report_detail
