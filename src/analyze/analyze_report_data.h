#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::analyze::report_detail {

enum class Band { kIdle, kLoad, kCooldown };

struct TickRow {
    std::int64_t tick = 0;
    std::string wall_clock;
    double elapsed_s = 0.0;
    Band band = Band::kIdle;
    std::optional<double> cpu_tctl_c;
    std::optional<double> gpu_memjn_c;
    std::optional<double> gpu_envelope_c;
    std::optional<double> loop_achieved_interval_ms;
    std::optional<double> loop_work_duration_ms;
    std::optional<double> loop_slip_ms;
    std::optional<std::int64_t> loop_overrun;
    std::optional<double> process_cpu_pct;
    std::optional<std::int64_t> process_working_set_bytes;
    std::optional<std::int64_t> process_private_bytes;
};

struct BandPercentiles {
    int n = 0;
    std::optional<double> cpu_tctl_p50, cpu_tctl_p90, cpu_tctl_max;
    std::optional<double> gpu_memjn_p50, gpu_memjn_p90, gpu_memjn_max;
    std::optional<double> gpu_env_p50, gpu_env_p90, gpu_env_max;
};

// Percentile set for a single metric (nearest-rank p50/p90/p95/p99/max + mean).
struct PercentileSet {
    int n = 0;
    std::optional<double> p50, p90, p95, p99, max, avg;
};

// Loop-timing and process-resource percentiles over all ticks of a run.
struct TimingResourceStats {
    PercentileSet loop_achieved_interval_ms;
    PercentileSet loop_work_duration_ms;
    PercentileSet loop_slip_ms;
    PercentileSet process_cpu_pct;
    PercentileSet process_working_set_bytes;
    PercentileSet process_private_bytes;
    int overrun_count = 0;
};

struct GpuChannelAtPeak {
    int channel = 0;
    std::optional<double> setpoint_at_peak_pct;
    std::optional<double> setpoint_during_load_p90;
    std::optional<double> setpoint_during_load_max;
    int load_row_count = 0;
};

// GPU-envelope peak and (when a threshold is supplied) threshold-crossing
// summary for native analyze report output.
struct GpuResponseSummary {
    bool has_peak = false;
    std::int64_t peak_row_number = 0;  // 1-based ordinal among ticks
    double peak_elapsed_s = 0.0;
    std::optional<double> peak_value_c;
    std::optional<double> threshold_c;
    std::optional<std::int64_t> above_threshold_rows;
    std::optional<double> above_threshold_seconds;
    std::optional<double> time_to_threshold_s;
    std::vector<GpuChannelAtPeak> channels;
};

struct ChannelStats {
    std::vector<double> setpoint_pct;
    std::vector<double> duty_pct;
    std::vector<double> rpm;
    double max_thermal_pressure_boost_pct = 0.0;
    double max_midband_pressure_boost_pct = 0.0;
    double max_gpu_airflow_boost_pct = 0.0;
    double max_cpu_low_soak_boost_pct = 0.0;
    // Max over rows of (sum of the 4 stage boosts present in the row +
    // low_band_effective-or-stage). This preserves the former raw-CSV
    // analyzer rule after analysis moved fully native.
    double response_boost_total = 0.0;
    std::map<std::string, int> primary_source_counts;
    std::map<std::string, int> response_source_counts;
    std::map<std::string, int> write_reason_counts;
    std::optional<std::int64_t> total_writes_min;
    std::optional<std::int64_t> total_writes_max;
    int reversals = 0;
    int mode_leave_ticks = 0;
    std::optional<double> last_setpoint;
    int last_direction = 0;  // -1, 0, +1
};

// One de-duplicated RAPL package-energy window (the logger mirrors one window
// across intervening ticks; the analyzer collapses by cpu_power_sample_id).
struct PackageEnergyWindow {
    double window_ms = 0.0;
    double delta_uj = 0.0;
};

// FEAT-0006 (REQ-CPUEFF-02) derived package-power evidence. avg_watts is the
// time-weighted average (total energy / total window time) over distinct
// sample-id windows - NOT a mean of per-window watts. nullopt avg_watts means
// no valid window was ingested (RAPL off/unavailable, or old archive): the
// report says "unavailable", never a false zero. acquisition_counts is the raw
// provenance breakdown over all ticks of the run.
struct PackagePowerSummary {
    int window_count = 0;
    double total_energy_j = 0.0;
    double total_window_s = 0.0;
    std::optional<double> avg_watts;
    std::optional<double> watts_p50;
    std::optional<double> watts_p90;
    std::optional<double> watts_max;
    std::map<std::string, int> acquisition_counts;
};

// One de-duplicated APERF/MPERF cycle window (the logger mirrors one window
// across intervening ticks; the analyzer collapses by cpu_cycles_sample_id).
struct CycleEvidenceWindow {
    double window_ms = 0.0;
    double d_aperf = 0.0;
    double d_mperf = 0.0;
};

// FEAT-0006 (REQ-CPUEFF-01/-03) derived APERF/MPERF cycle evidence.
// aperf_mperf_ratio is the cycle-weighted aggregate (total dAPERF / total
// dMPERF) over distinct sample-id windows - NOT a mean of per-window ratios.
// effective_mhz = ratio x P0; no logged field or document fixes a P0 source,
// so it is derived only when the operator passes --p0-mhz, and p0_mhz echoes
// that input so the report shows the reference it used. nullopt ratio means
// no valid window was ingested (cycles off/unavailable, or old archive): the
// report says "unavailable", never a false zero. acquisition_counts is the
// raw provenance breakdown over all ticks of the run.
struct CpuCyclesSummary {
    int window_count = 0;
    double total_aperf_cycles = 0.0;
    double total_mperf_cycles = 0.0;
    double total_window_s = 0.0;
    std::optional<double> aperf_mperf_ratio;
    std::optional<double> ratio_p50;
    std::optional<double> ratio_p90;
    std::optional<double> ratio_max;
    std::optional<double> p0_mhz;
    std::optional<double> effective_mhz;
    // FEAT-0006 all-core rollup only: the max/min contributing-core count over
    // the package sweep windows, so a partial (<32-core) sweep is auditable and
    // the Sigma-dAPERF work numerator is not misread as a full-package figure.
    // Left nullopt for the per-core (core-0) summary, which has no core count.
    std::optional<std::int64_t> contributing_cores_max;
    std::optional<std::int64_t> contributing_cores_min;
    std::map<std::string, int> acquisition_counts;
};

// FEAT-0020 (REQ-PWRLOG-02/-05) derived GPU board-power evidence. gpu_power_mw
// is INSTANTANEOUS board milliwatts (not an accumulating energy counter), so this
// summarizes the per-sample distribution (mean + nearest-rank p50/p90/max) over
// distinct gpu_power_sample_id values - deliberately NOT the time-weighted
// Sigma-energy/Sigma-window integral used for CPU package power. nullopt avg_mw
// means no valid sample was ingested (GPU power off/unavailable, or old archive):
// the report says "unavailable", never a false zero. acquisition_counts is the
// raw provenance breakdown over all ticks of the run.
struct GpuPowerSummary {
    int sample_count = 0;
    std::optional<double> avg_mw;
    std::optional<double> mw_p50;
    std::optional<double> mw_p90;
    std::optional<double> mw_max;
    std::map<std::string, int> acquisition_counts;
};

// FEAT-0021 (REQ-GPUCTX-05) derived GPU workload-context evidence. Samples are
// de-duplicated by gpu_context_sample_id because the control loop mirrors a
// cached context sample across rows; sample_age_ms is summarized over rows to
// show cache staleness. Empty stats mean the archive lacks context or the
// reader reported unavailable.
struct GpuContextSummary {
    int sample_count = 0;
    PercentileSet sample_age_ms;
    PercentileSet util_gpu_pct;
    PercentileSet util_mem_pct;
    PercentileSet clock_graphics_mhz;
    PercentileSet clock_memory_mhz;
    PercentileSet vram_used_mb;
    PercentileSet vram_total_mb;
    std::map<std::string, int> pstate_counts;
    std::map<std::string, int> acquisition_counts;
};

struct RuntimeManifestEvidence {
    std::filesystem::path config_path;
    std::string config_sha256;
    std::filesystem::path runtime_policy_path;
    std::string runtime_policy_sha256;
    std::filesystem::path events_path;
    std::filesystem::path csv_latest_path;
    std::optional<std::int64_t> csv_latest_row_count;
};

// Aggregated state assembled before output. Both emitters consume this; the
// main function populates it from the SQLite queries.
struct ReportData {
    std::int64_t run_id = 0;
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
    RuntimeManifestEvidence manifest_evidence;
    std::vector<TickRow> ticks;
    BandPercentiles idle;
    BandPercentiles load;
    BandPercentiles cool;
    std::map<int, ChannelStats> channels;
    std::optional<std::int64_t> onset_tick;
    std::optional<std::int64_t> response_tick;
    std::optional<double> response_delay_s;
    GpuResponseSummary gpu_response;
    TimingResourceStats timing_resources;
    PackagePowerSummary package_power;
    CpuCyclesSummary cpu_cycles;
    CpuCyclesSummary cpu_cycles_allcore;
    GpuPowerSummary gpu_power;
    GpuContextSummary gpu_context;
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;
    std::map<std::string, int> event_severity_counts;
    std::map<std::string, int> event_error_code_counts;
};

std::optional<double> Percentile(std::vector<double> values, double pct);
std::optional<double> Median(std::vector<double> values);
std::optional<double> Mean(const std::vector<double>& values);
BandPercentiles SummariseBand(const std::vector<TickRow>& ticks, Band band);

// Pure: time-weighted average package power + per-window watt distribution from
// already-deduplicated windows (one per sample id). acquisition_counts is
// provenance, copied through unchanged. Empty windows -> avg_watts nullopt.
PackagePowerSummary ComputePackagePower(
    const std::vector<PackageEnergyWindow>& windows,
    std::map<std::string, int> acquisition_counts);

// Pure: cycle-weighted APERF/MPERF ratio + per-window ratio distribution from
// already-deduplicated windows (one per sample id), and effective MHz when a
// positive p0_mhz is supplied. acquisition_counts is provenance, copied
// through unchanged. Empty windows -> aperf_mperf_ratio nullopt.
CpuCyclesSummary ComputeCpuCycles(
    const std::vector<CycleEvidenceWindow>& windows,
    std::map<std::string, int> acquisition_counts,
    std::optional<double> p0_mhz);

// Pure: GPU board-power distribution (mean + nearest-rank p50/p90/max) from the
// already-deduplicated instantaneous milliwatt samples (one per sample id).
// acquisition_counts is provenance, copied through unchanged. Empty samples ->
// avg_mw nullopt. This is a mean of instantaneous samples, NOT a Sigma-energy
// integral, because gpu_power_mw is an instantaneous reading.
GpuPowerSummary ComputeGpuPower(
    std::vector<double> sample_mw,
    std::map<std::string, int> acquisition_counts);

}  // namespace svg_mb_control::analyze::report_detail
