#include "analyze_report_data.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

}  // namespace svg_mb_control::analyze::report_detail
