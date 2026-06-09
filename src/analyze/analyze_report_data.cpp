#include "analyze_report_data.h"

#include "rapl_energy.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace svg_mb_control::analyze::report_detail {

// Nearest-rank percentile on values sorted ascending: pX is the value at
// index round((X/100) * (n - 1)). p100 is the maximum. Returns nullopt for
// an empty sample.
std::optional<double> Percentile(std::vector<double> values, double pct) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    const double scaled =
        (pct / 100.0) * static_cast<double>(values.size() - 1);
    auto index = static_cast<std::size_t>(std::llround(scaled));
    if (index >= values.size()) {
        index = values.size() - 1;
    }
    return values[index];
}

std::optional<double> Median(std::vector<double> values) {
    return Percentile(std::move(values), 50.0);
}

std::optional<double> Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return std::nullopt;
    }
    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

BandPercentiles SummariseBand(const std::vector<TickRow>& ticks, Band band) {
    std::vector<double> cpu, mem, env;
    for (const auto& t : ticks) {
        if (t.band != band) {
            continue;
        }
        if (t.cpu_tctl_c) cpu.push_back(*t.cpu_tctl_c);
        if (t.gpu_memjn_c) mem.push_back(*t.gpu_memjn_c);
        if (t.gpu_envelope_c) env.push_back(*t.gpu_envelope_c);
    }
    BandPercentiles bp;
    for (const auto& t : ticks) {
        if (t.band == band) ++bp.n;
    }
    bp.cpu_tctl_p50 = Percentile(cpu, 50.0);
    bp.cpu_tctl_p90 = Percentile(cpu, 90.0);
    bp.cpu_tctl_max = Percentile(cpu, 100.0);
    bp.gpu_memjn_p50 = Percentile(mem, 50.0);
    bp.gpu_memjn_p90 = Percentile(mem, 90.0);
    bp.gpu_memjn_max = Percentile(mem, 100.0);
    bp.gpu_env_p50 = Percentile(env, 50.0);
    bp.gpu_env_p90 = Percentile(env, 90.0);
    bp.gpu_env_max = Percentile(env, 100.0);
    return bp;
}

PackagePowerSummary ComputePackagePower(
    const std::vector<PackageEnergyWindow>& windows,
    std::map<std::string, int> acquisition_counts) {
    PackagePowerSummary out;
    out.acquisition_counts = std::move(acquisition_counts);
    std::vector<double> per_window_watts;
    per_window_watts.reserve(windows.size());
    for (const auto& w : windows) {
        const double watts = rapl::AveragePackageWatts(w.delta_uj, w.window_ms);
        // The log-time guard already blanked implausible windows; skip any
        // non-finite straggler so totals and percentiles stay clean.
        if (!std::isfinite(watts)) {
            continue;
        }
        ++out.window_count;
        out.total_energy_j += w.delta_uj / 1.0e6;
        out.total_window_s += w.window_ms / 1.0e3;
        per_window_watts.push_back(watts);
    }
    if (out.window_count > 0 && out.total_window_s > 0.0) {
        out.avg_watts = out.total_energy_j / out.total_window_s;
    }
    out.watts_p50 = Percentile(per_window_watts, 50.0);
    out.watts_p90 = Percentile(per_window_watts, 90.0);
    out.watts_max = Percentile(per_window_watts, 100.0);
    return out;
}

}  // namespace svg_mb_control::analyze::report_detail
