#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace svg_mb_control::analyze {

// Options for the offline `analyze report` summary. The report is read-only;
// it never opens the runtime, writes fans, or modifies the database.
struct ReportOptions {
    std::filesystem::path runtime_home;
    std::filesystem::path db_path;
    // Run selection. At most one of run_id / session_start is set; when both
    // are empty the most recent run (highest session_start, then highest id)
    // is used.
    std::optional<std::int64_t> run_id;
    std::optional<std::string> session_start;
    // Ticks whose elapsed time from the first tick is below idle_seconds form
    // the idle band. Default 300 s matches the 5-minute idle baseline the
    // evaluation passes in docs/response-evaluation-tuning-plan.md prescribe.
    std::uint32_t idle_seconds = 300u;
    // A tick is "under load" once cpu_tctl_c or gpu_envelope_c reaches this
    // value. Default 75 C matches the CPU response watch line in the plan.
    double load_threshold_c = 75.0;
    // Optional GPU-envelope threshold for the GPU-response block's
    // threshold-crossing metrics. When unset, only the peak + per-channel
    // setpoint-at-peak are reported.
    std::optional<double> gpu_load_threshold_c;
    // Optional P0 (base) frequency in MHz for the cpu_cycles block. The
    // APERF/MPERF ratio is derived from logged data alone; effective MHz =
    // ratio x P0 needs this operator-supplied reference because no logged
    // field or document fixes a P0 source. When unset, effective_mhz is
    // reported unavailable (never guessed).
    std::optional<double> p0_mhz;
    std::filesystem::path out_path;
    std::filesystem::path manifest_out_path;
    std::filesystem::path decision_record_out_path;
    bool decision_record_auto = false;
    bool no_decision_record = false;
    std::string profile;
    std::string notes;
    std::string hypothesis;
    std::string decision;
    bool as_json = false;
    bool quiet = false;
};

int RunAnalyzeReport(const ReportOptions& options);

}  // namespace svg_mb_control::analyze
