#include "analyze_report_emit.h"

#include "file_hash.h"
#include "json_io.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace svg_mb_control::analyze::report_detail {

namespace {

nlohmann::json OptStringToJson(const std::optional<std::string>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json OptIntToJson(const std::optional<std::int64_t>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json PathToJson(const std::filesystem::path& path) {
    return path.empty() ? nlohmann::json(nullptr)
                        : nlohmann::json(path.string());
}

nlohmann::json OptToJson(const std::optional<double>& v) {
    if (!v) {
        return nullptr;
    }
    return *v;
}

std::string OptToText(const std::optional<double>& v) {
    if (!v) {
        return "n/a";
    }
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << *v;
    return os.str();
}

std::string CountsToText(const std::map<std::string, int>& counts) {
    if (counts.empty()) {
        return "none";
    }
    std::ostringstream os;
    bool first = true;
    for (const auto& [name, count] : counts) {
        if (!first) {
            os << ' ';
        }
        first = false;
        os << name << '=' << count;
    }
    return os.str();
}

std::string TextOrNa(const std::optional<std::string>& value) {
    return value && !value->empty() ? *value : "n/a";
}

std::string IntOrNa(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "n/a";
}

std::string PathOrNa(const std::filesystem::path& path) {
    return path.empty() ? "n/a" : path.string();
}

nlohmann::json ArtifactJson(const std::filesystem::path& path,
                            const std::string& schema = {},
                            const std::string& sha256_override = {}) {
    nlohmann::json artifact;
    artifact["path"] = PathToJson(path);
    if (!schema.empty()) {
        artifact["schema"] = schema;
    }
    std::string sha256 = sha256_override;
    if (sha256.empty() && !path.empty()) {
        sha256 = svg_mb_control::Sha256FileHex(path);
    }
    artifact["sha256"] =
        sha256.empty() ? nlohmann::json(nullptr) : nlohmann::json(sha256);
    return artifact;
}

nlohmann::json BuildAnalysisManifest(const ReportOptions& options,
                                     const ReportData& data,
                                     const OutputArtifactPaths& outputs) {
    nlohmann::json doc;
    doc["schema"] = "svg_mb_control.analysis_manifest.v1";
    doc["run"] = {
        {"id", data.run_id},
        {"session_start", data.session_start},
        {"mode", data.mode},
        {"status", data.status},
        {"tool_version", OptStringToJson(data.tool_version)},
        {"git_hash", OptStringToJson(data.git_hash)},
        {"row_count_declared", OptIntToJson(data.row_count_declared)},
        {"row_count_ingested", OptIntToJson(data.row_count_ingested)},
        {"event_count_declared", OptIntToJson(data.event_count_declared)},
        {"event_count_ingested", OptIntToJson(data.event_count_ingested)},
        {"last_update", OptStringToJson(data.last_update)},
    };
    doc["params"] = {
        {"idle_seconds", options.idle_seconds},
        {"load_threshold_c", options.load_threshold_c},
        {"profile", options.profile.empty() ? nlohmann::json(nullptr)
                                             : nlohmann::json(options.profile)},
        {"hypothesis",
         options.hypothesis.empty() ? nlohmann::json(nullptr)
                                    : nlohmann::json(options.hypothesis)},
        {"decision",
         options.decision.empty() ? nlohmann::json(nullptr)
                                  : nlohmann::json(options.decision)},
        {"notes", options.notes.empty() ? nlohmann::json(nullptr)
                                        : nlohmann::json(options.notes)},
    };
    doc["source_artifacts"] = {
        {"csv", ArtifactJson(data.csv_archive_path,
                             "svg_mb_control.log.v1")},
        {"runtime_manifest",
         ArtifactJson(data.manifest_path,
                      "svg_mb_control.runtime_log_manifest.v1")},
    };
    if (!data.manifest_evidence.config_path.empty()) {
        doc["source_artifacts"]["config"] = ArtifactJson(
            data.manifest_evidence.config_path, {},
            data.manifest_evidence.config_sha256);
    }
    if (!data.manifest_evidence.runtime_policy_path.empty()) {
        doc["source_artifacts"]["runtime_policy"] = ArtifactJson(
            data.manifest_evidence.runtime_policy_path, {},
            data.manifest_evidence.runtime_policy_sha256);
    }
    if (!data.manifest_evidence.events_path.empty()) {
        doc["source_artifacts"]["events"] = ArtifactJson(
            data.manifest_evidence.events_path,
            "svg_mb_control.event.v1");
    }

    doc["outputs"] = nlohmann::json::object();
    if (!outputs.report_path.empty()) {
        doc["outputs"]["report"] = ArtifactJson(outputs.report_path);
    }
    if (!outputs.decision_record_path.empty()) {
        doc["outputs"]["decision_record"] =
            ArtifactJson(outputs.decision_record_path);
    }

    doc["summary"] = {
        {"ticks", static_cast<std::int64_t>(data.ticks.size())},
        {"elapsed_s", data.ticks.back().elapsed_s},
        {"response",
         {
             {"load_onset_tick",
              data.onset_tick ? nlohmann::json(*data.onset_tick)
                              : nlohmann::json(nullptr)},
             {"first_setpoint_increase_tick",
              data.response_tick ? nlohmann::json(*data.response_tick)
                                 : nlohmann::json(nullptr)},
             {"response_delay_s", OptToJson(data.response_delay_s)},
         }},
        {"robustness",
         {
             {"authority_reasserted", data.authority_reasserted},
             {"write_failures", data.write_failures},
             {"restore_failures", data.restore_failures},
         }},
        {"diagnostic_flags", BuildDiagnosticFlags(options, data)},
        {"event_severity_counts", data.event_severity_counts},
        {"event_error_code_counts", data.event_error_code_counts},
    };
    return doc;
}

}  // namespace

std::vector<std::string> BuildDiagnosticFlags(const ReportOptions& options,
                                              const ReportData& data) {
    std::vector<std::string> flags;
    if (data.onset_tick && !data.response_tick) {
        flags.push_back("no_setpoint_response_after_load_threshold");
    }
    if (data.response_delay_s && *data.response_delay_s > 10.0) {
        flags.push_back("slow_setpoint_response_after_load_threshold");
    }
    if (data.authority_reasserted > 0) {
        flags.push_back("authority_reasserted_during_run");
    }
    if (data.write_failures > 0) {
        flags.push_back("write_failures_present");
    }
    if (data.restore_failures > 0) {
        flags.push_back("restore_failures_present");
    }

    const bool hot =
        (data.load.cpu_tctl_max &&
         *data.load.cpu_tctl_max >= options.load_threshold_c) ||
        (data.load.gpu_env_max &&
         *data.load.gpu_env_max >= options.load_threshold_c);
    std::optional<double> max_channel_p90;
    for (const auto& [_, cs] : data.channels) {
        const auto p90 = Percentile(cs.setpoint_pct, 90.0);
        if (p90 && (!max_channel_p90 || *p90 > *max_channel_p90)) {
            max_channel_p90 = p90;
        }
    }
    if (hot && max_channel_p90 && *max_channel_p90 < 35.0) {
        flags.push_back("hot_but_low_channel_setpoint");
    }
    return flags;
}

void EmitJsonReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out) {
    nlohmann::json doc;
    doc["run"] = {
        {"id", data.run_id},
        {"session_start", data.session_start},
        {"mode", data.mode},
        {"status", data.status},
        {"manifest_path", PathToJson(data.manifest_path)},
        {"csv_archive_path", PathToJson(data.csv_archive_path)},
        {"tool_version", OptStringToJson(data.tool_version)},
        {"git_hash", OptStringToJson(data.git_hash)},
        {"row_count_declared", OptIntToJson(data.row_count_declared)},
        {"row_count_ingested", OptIntToJson(data.row_count_ingested)},
        {"event_count_declared", OptIntToJson(data.event_count_declared)},
        {"event_count_ingested", OptIntToJson(data.event_count_ingested)},
        {"ticks", static_cast<std::int64_t>(data.ticks.size())},
        {"elapsed_s", data.ticks.back().elapsed_s},
    };
    doc["params"] = {
        {"idle_seconds", options.idle_seconds},
        {"load_threshold_c", options.load_threshold_c},
        {"percentile_method",
         "nearest-rank on sorted ascending, p100=max"},
    };
    auto band_json = [](const BandPercentiles& b) {
        return nlohmann::json{
            {"n", b.n},
            {"cpu_tctl_c",
             {{"p50", OptToJson(b.cpu_tctl_p50)},
              {"p90", OptToJson(b.cpu_tctl_p90)},
              {"max", OptToJson(b.cpu_tctl_max)}}},
            {"gpu_memjn_c",
             {{"p50", OptToJson(b.gpu_memjn_p50)},
              {"p90", OptToJson(b.gpu_memjn_p90)},
              {"max", OptToJson(b.gpu_memjn_max)}}},
            {"gpu_envelope_c",
             {{"p50", OptToJson(b.gpu_env_p50)},
              {"p90", OptToJson(b.gpu_env_p90)},
              {"max", OptToJson(b.gpu_env_max)}}},
        };
    };
    doc["bands"] = {
        {"idle", band_json(data.idle)},
        {"load", band_json(data.load)},
        {"cooldown", band_json(data.cool)},
    };
    doc["channels"] = nlohmann::json::array();
    for (const auto& [ch, cs] : data.channels) {
        std::int64_t writes = 0;
        if (cs.total_writes_min && cs.total_writes_max) {
            writes = *cs.total_writes_max - *cs.total_writes_min;
        }
        doc["channels"].push_back({
            {"channel", ch},
            {"setpoint_pct",
             {{"p50", OptToJson(Percentile(cs.setpoint_pct, 50.0))},
              {"p90", OptToJson(Percentile(cs.setpoint_pct, 90.0))},
              {"max", OptToJson(Percentile(cs.setpoint_pct, 100.0))}}},
            {"duty_pct",
             {{"p50", OptToJson(Percentile(cs.duty_pct, 50.0))},
              {"p90", OptToJson(Percentile(cs.duty_pct, 90.0))},
              {"max", OptToJson(Percentile(cs.duty_pct, 100.0))}}},
            {"rpm",
             {{"p50", OptToJson(Percentile(cs.rpm, 50.0))},
              {"p90", OptToJson(Percentile(cs.rpm, 90.0))},
              {"max", OptToJson(Percentile(cs.rpm, 100.0))}}},
            {"max_thermal_pressure_boost_pct",
             cs.max_thermal_pressure_boost_pct},
            {"max_midband_pressure_boost_pct",
             cs.max_midband_pressure_boost_pct},
            {"max_gpu_airflow_boost_pct",
             cs.max_gpu_airflow_boost_pct},
            {"max_cpu_low_soak_boost_pct",
             cs.max_cpu_low_soak_boost_pct},
            {"primary_temp_source_counts", cs.primary_source_counts},
            {"response_source_counts", cs.response_source_counts},
            {"write_reason_counts", cs.write_reason_counts},
            {"writes", writes},
            {"reversals", cs.reversals},
            {"mode_leave_ticks", cs.mode_leave_ticks},
        });
    }
    doc["response"] = {
        {"load_onset_tick",
         data.onset_tick ? nlohmann::json(*data.onset_tick)
                         : nlohmann::json()},
        {"first_setpoint_increase_tick",
         data.response_tick ? nlohmann::json(*data.response_tick)
                            : nlohmann::json()},
        {"response_delay_s", OptToJson(data.response_delay_s)},
    };
    doc["robustness"] = {
        {"authority_reasserted", data.authority_reasserted},
        {"write_failures", data.write_failures},
        {"restore_failures", data.restore_failures},
    };
    doc["events"] = {
        {"severity_counts", data.event_severity_counts},
        {"error_code_counts", data.event_error_code_counts},
    };
    doc["diagnostic_flags"] = BuildDiagnosticFlags(options, data);
    out << doc.dump(2) << '\n';
}

void EmitTextReport(const ReportOptions& options,
                    const ReportData& data,
                    std::ostream& out) {
    std::ostringstream os;
    os << "analyze report: run_id=" << data.run_id
       << " session_start=" << data.session_start << " mode=" << data.mode
       << " status=" << data.status << '\n';
    os << "  ticks=" << data.ticks.size()
       << " elapsed_s=" << data.ticks.back().elapsed_s
       << " idle_seconds=" << options.idle_seconds
       << " load_threshold_c=" << options.load_threshold_c << '\n';
    os << "  rows declared/ingested=" << IntOrNa(data.row_count_declared)
       << "/" << IntOrNa(data.row_count_ingested)
       << " events declared/ingested="
       << IntOrNa(data.event_count_declared) << "/"
       << IntOrNa(data.event_count_ingested) << '\n';
    os << "  artifacts: csv=" << PathOrNa(data.csv_archive_path)
       << " manifest=" << PathOrNa(data.manifest_path) << '\n';
    os << "  band sample counts: idle=" << data.idle.n
       << " load=" << data.load.n << " cooldown=" << data.cool.n << '\n';
    os << "  percentile method: nearest-rank on sorted ascending, p100=max\n";
    auto print_band = [&os](const char* name, const BandPercentiles& b) {
        os << "[" << name << "] cpu_tctl_c p50=" << OptToText(b.cpu_tctl_p50)
           << " p90=" << OptToText(b.cpu_tctl_p90)
           << " max=" << OptToText(b.cpu_tctl_max)
           << "  gpu_memjn_c p50=" << OptToText(b.gpu_memjn_p50)
           << " p90=" << OptToText(b.gpu_memjn_p90)
           << " max=" << OptToText(b.gpu_memjn_max)
           << "  gpu_envelope_c p50=" << OptToText(b.gpu_env_p50)
           << " p90=" << OptToText(b.gpu_env_p90)
           << " max=" << OptToText(b.gpu_env_max) << '\n';
    };
    print_band("idle", data.idle);
    print_band("load", data.load);
    print_band("cooldown", data.cool);
    os << "channels:\n";
    for (const auto& [ch, cs] : data.channels) {
        std::int64_t writes = 0;
        if (cs.total_writes_min && cs.total_writes_max) {
            writes = *cs.total_writes_max - *cs.total_writes_min;
        }
        os << "  ch" << ch << " setpoint_pct p50="
           << OptToText(Percentile(cs.setpoint_pct, 50.0))
           << " p90=" << OptToText(Percentile(cs.setpoint_pct, 90.0))
           << " max=" << OptToText(Percentile(cs.setpoint_pct, 100.0))
           << "  duty_pct p50=" << OptToText(Percentile(cs.duty_pct, 50.0))
           << " p90=" << OptToText(Percentile(cs.duty_pct, 90.0))
           << " max=" << OptToText(Percentile(cs.duty_pct, 100.0))
           << "  rpm p50=" << OptToText(Percentile(cs.rpm, 50.0))
           << " p90=" << OptToText(Percentile(cs.rpm, 90.0))
           << " max=" << OptToText(Percentile(cs.rpm, 100.0)) << '\n';
        os << "       thermal_pressure_boost_pct max="
           << cs.max_thermal_pressure_boost_pct
           << "  midband_pressure_boost_pct max="
           << cs.max_midband_pressure_boost_pct
           << "  gpu_airflow_boost_pct max="
           << cs.max_gpu_airflow_boost_pct
           << "  cpu_low_soak_boost_pct max="
           << cs.max_cpu_low_soak_boost_pct << "  writes=" << writes
           << "  reversals=" << cs.reversals
           << "  mode_leave_ticks=" << cs.mode_leave_ticks << '\n';
        os << "       primary_temp_source_counts "
           << CountsToText(cs.primary_source_counts) << '\n';
        os << "       response_source_counts "
           << CountsToText(cs.response_source_counts) << '\n';
        os << "       write_reason_counts "
           << CountsToText(cs.write_reason_counts) << '\n';
    }
    os << "response: load_onset_tick="
       << (data.onset_tick ? std::to_string(*data.onset_tick) : "n/a")
       << " first_setpoint_increase_tick="
       << (data.response_tick ? std::to_string(*data.response_tick) : "n/a")
       << " response_delay_s=" << OptToText(data.response_delay_s) << '\n';
    os << "robustness: authority_reasserted=" << data.authority_reasserted
       << " write_failures=" << data.write_failures
       << " restore_failures=" << data.restore_failures << '\n';
    os << "events: severity_counts="
       << CountsToText(data.event_severity_counts)
       << " error_code_counts="
       << CountsToText(data.event_error_code_counts) << '\n';
    os << "diagnostic_flags: "
       << CountsToText([&]() {
              std::map<std::string, int> flag_counts;
              for (const auto& flag : BuildDiagnosticFlags(options, data)) {
                  flag_counts[flag] = 1;
              }
              return flag_counts;
          }()) << '\n';
    out << os.str();
}

std::string BuildDecisionRecord(const ReportOptions& options,
                                const ReportData& data) {
    std::ostringstream os;
    os << "# Control Run Decision Record\n\n";
    os << "- Run ID: " << data.run_id << '\n';
    os << "- Session: " << data.session_start << '\n';
    os << "- Mode: " << data.mode << '\n';
    os << "- Status: " << data.status << '\n';
    os << "- Profile: "
       << (options.profile.empty() ? "n/a" : options.profile) << '\n';
    os << "- Hypothesis: "
       << (options.hypothesis.empty() ? "n/a" : options.hypothesis) << '\n';
    os << "- Decision: "
       << (options.decision.empty() ? "n/a" : options.decision) << '\n';
    if (!options.notes.empty()) {
        os << "- Notes: " << options.notes << '\n';
    }

    os << "\n## Source Artifacts\n\n";
    os << "- CSV: " << PathOrNa(data.csv_archive_path)
       << " sha256=" << TextOrNa(
              data.csv_archive_path.empty()
                  ? std::optional<std::string>()
                  : std::optional<std::string>(
                        svg_mb_control::Sha256FileHex(data.csv_archive_path)))
       << '\n';
    os << "- Runtime manifest: " << PathOrNa(data.manifest_path)
       << " sha256=" << TextOrNa(
              data.manifest_path.empty()
                  ? std::optional<std::string>()
                  : std::optional<std::string>(
                        svg_mb_control::Sha256FileHex(data.manifest_path)))
       << '\n';
    if (!data.manifest_evidence.config_path.empty()) {
        os << "- Config: " << data.manifest_evidence.config_path.string()
           << " sha256="
           << (data.manifest_evidence.config_sha256.empty()
                   ? svg_mb_control::Sha256FileHex(
                         data.manifest_evidence.config_path)
                   : data.manifest_evidence.config_sha256)
           << '\n';
    }
    if (!data.manifest_evidence.runtime_policy_path.empty()) {
        os << "- Runtime policy: "
           << data.manifest_evidence.runtime_policy_path.string()
           << " sha256="
           << (data.manifest_evidence.runtime_policy_sha256.empty()
                   ? svg_mb_control::Sha256FileHex(
                         data.manifest_evidence.runtime_policy_path)
                   : data.manifest_evidence.runtime_policy_sha256)
           << '\n';
    }

    os << "\n## Counts\n\n";
    os << "- Rows declared/ingested: " << IntOrNa(data.row_count_declared)
       << "/" << IntOrNa(data.row_count_ingested) << '\n';
    os << "- Events declared/ingested: "
       << IntOrNa(data.event_count_declared) << "/"
       << IntOrNa(data.event_count_ingested) << '\n';
    os << "- Tool/git: " << TextOrNa(data.tool_version) << "/"
       << TextOrNa(data.git_hash) << '\n';

    os << "\n## Response\n\n";
    os << "- Load onset tick: "
       << (data.onset_tick ? std::to_string(*data.onset_tick) : "n/a")
       << '\n';
    os << "- First setpoint increase tick: "
       << (data.response_tick ? std::to_string(*data.response_tick) : "n/a")
       << '\n';
    os << "- Response delay s: " << OptToText(data.response_delay_s) << '\n';
    os << "- Robustness: authority_reasserted="
       << data.authority_reasserted << " write_failures="
       << data.write_failures << " restore_failures="
       << data.restore_failures << '\n';

    const auto flags = BuildDiagnosticFlags(options, data);
    os << "\n## Diagnostic Flags\n\n";
    if (flags.empty()) {
        os << "- none\n";
    } else {
        for (const auto& flag : flags) {
            os << "- " << flag << '\n';
        }
    }

    os << "\n## Channel Attribution\n\n";
    for (const auto& [ch, cs] : data.channels) {
        os << "- ch" << ch << " setpoint_p90="
           << OptToText(Percentile(cs.setpoint_pct, 90.0))
           << " setpoint_max="
           << OptToText(Percentile(cs.setpoint_pct, 100.0))
           << " response_sources="
           << CountsToText(cs.response_source_counts)
           << " write_reasons=" << CountsToText(cs.write_reason_counts)
           << " primary_sources="
           << CountsToText(cs.primary_source_counts) << '\n';
    }

    os << "\n## Events\n\n";
    os << "- severity: " << CountsToText(data.event_severity_counts) << '\n';
    os << "- error_code: " << CountsToText(data.event_error_code_counts)
       << '\n';
    return os.str();
}

std::filesystem::path AutoDecisionRecordPath(
    const std::filesystem::path& report_path) {
    if (report_path.empty()) {
        return {};
    }
    std::filesystem::path filename =
        report_path.stem().string() + ".decision.md";
    return report_path.parent_path() / filename;
}

bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& content,
                   const char* label) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Error: cannot create " << label
                      << " parent directory " << parent.string() << ": "
                      << ec.message() << '\n';
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::cerr << "Error: cannot write " << label << ": "
                  << path.string() << '\n';
        return false;
    }
    stream << content;
    if (!stream) {
        std::cerr << "Error: failed while writing " << label << ": "
                  << path.string() << '\n';
        return false;
    }
    return true;
}

bool WriteAnalysisManifest(const ReportOptions& options,
                           const ReportData& data,
                           const OutputArtifactPaths& outputs,
                           const std::filesystem::path& path) {
    try {
        svg_mb_control::WriteJsonFileAtomic(
            path, BuildAnalysisManifest(options, data, outputs), 2);
    } catch (const std::exception& ex) {
        std::cerr << "Error: cannot write analysis manifest "
                  << path.string() << ": " << ex.what() << '\n';
        return false;
    }
    return true;
}

}  // namespace svg_mb_control::analyze::report_detail
