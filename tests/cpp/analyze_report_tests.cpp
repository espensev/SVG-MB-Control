// Unit tests for the pure helpers exposed by analyze_report_data.h,
// analyze_report_queries.h, and analyze_report_emit.h. The orchestrator
// path (RunAnalyzeReport) and all DB-bound queries are covered by the
// Python hermetic suite (tests/test_analyze_ingest.py); here we cover
// the in-memory math/processing primitives so they regress loudly if a
// future refactor moves them.

#include "analyze/analyze_report.h"

#include "test_helpers.h"

#include "analyze/analyze_report_data.h"
#include "analyze/analyze_report_emit.h"
#include "analyze/analyze_report_queries.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using svg_mb_control::analyze::ReportOptions;
using svg_mb_control::analyze::report_detail::AssignBands;
using svg_mb_control::analyze::report_detail::Band;
using svg_mb_control::analyze::report_detail::BandPercentiles;
using svg_mb_control::analyze::report_detail::BuildDiagnosticFlags;
using svg_mb_control::analyze::report_detail::ChannelStats;
using svg_mb_control::analyze::report_detail::ComputeElapsedTime;
using svg_mb_control::analyze::report_detail::ComputePackagePower;
using svg_mb_control::analyze::report_detail::Median;
using svg_mb_control::analyze::report_detail::PackageEnergyWindow;
using svg_mb_control::analyze::report_detail::PackagePowerSummary;
using svg_mb_control::analyze::report_detail::Percentile;
using svg_mb_control::analyze::report_detail::ReportData;
using svg_mb_control::analyze::report_detail::SummariseBand;
using svg_mb_control::analyze::report_detail::TickRow;

void ExpectEqualInt(int actual, int expected, const std::string& message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected " << expected
                  << " got " << actual << '\n';
    }
}

void ExpectHasValue(const std::optional<double>& v,
                    const std::string& message) {
    if (!v.has_value()) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected a value, got nullopt\n";
    }
}

void ExpectNullopt(const std::optional<double>& v,
                   const std::string& message) {
    if (v.has_value()) {
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected nullopt, got " << *v << '\n';
    }
}

constexpr double kEps = 1e-9;

void TestPercentile_Empty() {
    ExpectNullopt(Percentile({}, 50.0),
                  "Percentile of empty input is nullopt");
    ExpectNullopt(Median({}), "Median of empty input is nullopt");
}

void TestPercentile_Single() {
    const std::vector<double> v{5.0};
    auto p0 = Percentile(v, 0.0);
    auto p50 = Percentile(v, 50.0);
    auto p100 = Percentile(v, 100.0);
    ExpectHasValue(p0, "p0 of single-element");
    ExpectHasValue(p50, "p50 of single-element");
    ExpectHasValue(p100, "p100 of single-element");
    ExpectNear(*p0, 5.0, kEps, "p0 single == 5");
    ExpectNear(*p50, 5.0, kEps, "p50 single == 5");
    ExpectNear(*p100, 5.0, kEps, "p100 single == 5");
}

void TestPercentile_NearestRank() {
    // {1, 2, 3, 4, 5} — n=5, nearest-rank: index = round((pct/100) * 4).
    const std::vector<double> v{1.0, 2.0, 3.0, 4.0, 5.0};
    ExpectNear(*Percentile(v, 0.0), 1.0, kEps, "p0 of 5-ary == 1");
    ExpectNear(*Percentile(v, 50.0), 3.0, kEps, "p50 of 5-ary == 3");
    ExpectNear(*Percentile(v, 90.0), 5.0, kEps,
               "p90 of 5-ary == 5 (round(3.6))");
    ExpectNear(*Percentile(v, 100.0), 5.0, kEps,
               "p100 of 5-ary == 5 (max)");
}

void TestPercentile_ThreeAry() {
    // {1, 2, 3} — n=3, index = round((pct/100) * 2).
    const std::vector<double> v{1.0, 2.0, 3.0};
    ExpectNear(*Percentile(v, 0.0), 1.0, kEps, "p0 of 3-ary == 1");
    ExpectNear(*Percentile(v, 50.0), 2.0, kEps, "p50 of 3-ary == 2");
    ExpectNear(*Percentile(v, 100.0), 3.0, kEps, "p100 of 3-ary == 3");
}

void TestPercentile_Unsorted() {
    // Input intentionally unsorted; Percentile sorts internally before
    // ranking. p100 must still return the maximum.
    const std::vector<double> v{3.0, 1.0, 2.0};
    ExpectNear(*Percentile(v, 100.0), 3.0, kEps,
               "p100 of unsorted == max");
    ExpectNear(*Percentile(v, 0.0), 1.0, kEps,
               "p0 of unsorted == min");
}

void TestMedian_Matches50p() {
    const std::vector<double> v{10.0, 20.0, 30.0, 40.0, 50.0};
    ExpectNear(*Median(v), 30.0, kEps, "Median == p50");
}

TickRow MakeTick(std::int64_t index, Band band,
                 std::optional<double> cpu,
                 std::optional<double> gpu_env = std::nullopt,
                 std::optional<double> gpu_memjn = std::nullopt) {
    TickRow t;
    t.tick = index;
    t.band = band;
    t.cpu_tctl_c = cpu;
    t.gpu_envelope_c = gpu_env;
    t.gpu_memjn_c = gpu_memjn;
    return t;
}

void TestSummariseBand_Empty() {
    const std::vector<TickRow> ticks;
    const BandPercentiles bp = SummariseBand(ticks, Band::kLoad);
    ExpectEqualInt(bp.n, 0, "SummariseBand empty -> n=0");
    ExpectNullopt(bp.cpu_tctl_max, "SummariseBand empty -> cpu p100 nullopt");
    ExpectNullopt(bp.gpu_env_max, "SummariseBand empty -> gpu p100 nullopt");
}

void TestSummariseBand_FiltersByBand() {
    std::vector<TickRow> ticks;
    ticks.push_back(MakeTick(0, Band::kIdle, 50.0));
    ticks.push_back(MakeTick(1, Band::kLoad, 70.0));
    ticks.push_back(MakeTick(2, Band::kLoad, 80.0));
    ticks.push_back(MakeTick(3, Band::kCooldown, 60.0));

    const BandPercentiles load = SummariseBand(ticks, Band::kLoad);
    ExpectEqualInt(load.n, 2, "Load band n counts only load ticks");
    ExpectHasValue(load.cpu_tctl_p50, "Load band p50 has value");
    ExpectHasValue(load.cpu_tctl_max, "Load band p100 has value");
    ExpectNear(*load.cpu_tctl_max, 80.0, kEps, "Load p100 == 80");

    const BandPercentiles idle = SummariseBand(ticks, Band::kIdle);
    ExpectEqualInt(idle.n, 1, "Idle band n == 1");
    ExpectNear(*idle.cpu_tctl_max, 50.0, kEps, "Idle p100 == 50");
}

void TestSummariseBand_MissingOptionals() {
    // CPU values present for some ticks, nullopt for others. Percentiles
    // run over the non-null subset; bp.n still counts all band ticks.
    std::vector<TickRow> ticks;
    ticks.push_back(MakeTick(0, Band::kLoad, std::nullopt));
    ticks.push_back(MakeTick(1, Band::kLoad, 70.0));
    ticks.push_back(MakeTick(2, Band::kLoad, 80.0));

    const BandPercentiles load = SummariseBand(ticks, Band::kLoad);
    ExpectEqualInt(load.n, 3, "n counts all band ticks even with null cpu");
    ExpectHasValue(load.cpu_tctl_max,
                   "p100 derived from the present cpu values");
    ExpectNear(*load.cpu_tctl_max, 80.0, kEps,
               "p100 == max of present cpu values");
    ExpectNullopt(load.gpu_env_max,
                  "gpu envelope nullopt across all rows -> p100 nullopt");
}

void TestComputeElapsedTime_ParsedAdvances() {
    std::vector<TickRow> ticks;
    TickRow a, b, c;
    a.wall_clock = "2026-05-28T10:00:00";
    b.wall_clock = "2026-05-28T10:00:01";
    c.wall_clock = "2026-05-28T10:00:02";
    ticks.push_back(a);
    ticks.push_back(b);
    ticks.push_back(c);

    ComputeElapsedTime(ticks);

    ExpectNear(ticks[0].elapsed_s, 0.0, 1e-6,
               "elapsed_s[0] == 0 when first sample is parseable");
    ExpectNear(ticks[1].elapsed_s, 1.0, 1e-6,
               "elapsed_s[1] tracks 1-second wall_clock advance");
    ExpectNear(ticks[2].elapsed_s, 2.0, 1e-6,
               "elapsed_s[2] tracks 2-second wall_clock advance");
}

void TestComputeElapsedTime_StaticTimestampSynthetic() {
    // All wall_clocks identical: no advance is observed, fall back to the
    // synthetic 50 ms/tick stride so AssignBands has a usable time axis.
    std::vector<TickRow> ticks;
    for (int i = 0; i < 4; ++i) {
        TickRow t;
        t.wall_clock = "2026-05-28T10:00:00";
        ticks.push_back(t);
    }

    ComputeElapsedTime(ticks);

    ExpectNear(ticks[0].elapsed_s, 0.0, 1e-6, "synth elapsed_s[0] == 0");
    ExpectNear(ticks[1].elapsed_s, 0.05, 1e-6,
               "synth elapsed_s[1] == 50 ms stride");
    ExpectNear(ticks[2].elapsed_s, 0.10, 1e-6,
               "synth elapsed_s[2] == 100 ms");
    ExpectNear(ticks[3].elapsed_s, 0.15, 1e-6,
               "synth elapsed_s[3] == 150 ms");
}

void TestComputeElapsedTime_UnparseableSynthetic() {
    // Garbage wall_clock — no parsing, synthetic stride only.
    std::vector<TickRow> ticks;
    for (int i = 0; i < 3; ++i) {
        TickRow t;
        t.wall_clock = "not-a-timestamp";
        ticks.push_back(t);
    }

    ComputeElapsedTime(ticks);

    ExpectNear(ticks[0].elapsed_s, 0.0, 1e-6,
               "unparseable elapsed_s[0] == 0");
    ExpectNear(ticks[2].elapsed_s, 0.10, 1e-6,
               "unparseable elapsed_s[2] == 100 ms synthetic");
}

void TestAssignBands_AllIdle() {
    ReportOptions options;
    options.idle_seconds = 100u;
    options.load_threshold_c = 75.0;

    std::vector<TickRow> ticks;
    for (int i = 0; i < 5; ++i) {
        TickRow t;
        t.elapsed_s = static_cast<double>(i) * 10.0;  // 0..40, all < 100
        t.cpu_tctl_c = 50.0;
        ticks.push_back(t);
    }

    AssignBands(ticks, options);

    for (std::size_t i = 0; i < ticks.size(); ++i) {
        ExpectTrue(ticks[i].band == Band::kIdle,
                   "All-idle: tick " + std::to_string(i) + " is kIdle");
    }
}

void TestAssignBands_LoadThenCooldown() {
    ReportOptions options;
    options.idle_seconds = 100u;
    options.load_threshold_c = 75.0;

    std::vector<TickRow> ticks;
    // 3 idle ticks (elapsed < 100 s).
    for (int i = 0; i < 3; ++i) {
        TickRow t;
        t.elapsed_s = static_cast<double>(i) * 30.0;  // 0, 30, 60
        t.cpu_tctl_c = 50.0;
        ticks.push_back(t);
    }
    // 3 load ticks past idle threshold, hot (above 75 C).
    for (int i = 0; i < 3; ++i) {
        TickRow t;
        t.elapsed_s = 110.0 + static_cast<double>(i) * 10.0;
        t.cpu_tctl_c = 80.0;
        ticks.push_back(t);
    }
    // 2 cooldown ticks past idle threshold, below load threshold and
    // after the last hot tick.
    for (int i = 0; i < 2; ++i) {
        TickRow t;
        t.elapsed_s = 200.0 + static_cast<double>(i) * 10.0;
        t.cpu_tctl_c = 60.0;
        ticks.push_back(t);
    }

    AssignBands(ticks, options);

    ExpectTrue(ticks[0].band == Band::kIdle, "tick 0 idle");
    ExpectTrue(ticks[2].band == Band::kIdle, "tick 2 idle");
    ExpectTrue(ticks[3].band == Band::kLoad, "tick 3 load");
    ExpectTrue(ticks[5].band == Band::kLoad, "tick 5 last hot -> load");
    ExpectTrue(ticks[6].band == Band::kCooldown, "tick 6 cooldown");
    ExpectTrue(ticks[7].band == Band::kCooldown, "tick 7 cooldown");
}

void TestAssignBands_HotInIdleIgnored() {
    // A hot tick before idle_seconds doesn't qualify as last_hot, so
    // there's no cooldown band — everything past idle stays kLoad.
    ReportOptions options;
    options.idle_seconds = 100u;
    options.load_threshold_c = 75.0;

    std::vector<TickRow> ticks;
    TickRow hot_in_idle;
    hot_in_idle.elapsed_s = 50.0;
    hot_in_idle.cpu_tctl_c = 80.0;
    ticks.push_back(hot_in_idle);
    TickRow past_idle_cool;
    past_idle_cool.elapsed_s = 150.0;
    past_idle_cool.cpu_tctl_c = 60.0;
    ticks.push_back(past_idle_cool);
    TickRow past_idle_cool2;
    past_idle_cool2.elapsed_s = 200.0;
    past_idle_cool2.cpu_tctl_c = 60.0;
    ticks.push_back(past_idle_cool2);

    AssignBands(ticks, options);

    ExpectTrue(ticks[0].band == Band::kIdle,
               "Hot tick before idle threshold is still kIdle");
    ExpectTrue(ticks[1].band == Band::kLoad,
               "Past-idle tick is kLoad (no cooldown without hot anchor)");
    ExpectTrue(ticks[2].band == Band::kLoad,
               "Past-idle tick stays kLoad");
}

void TestBuildDiagnosticFlags_HotButLowSetpoint() {
    ReportOptions options;
    options.load_threshold_c = 75.0;

    ReportData data;
    data.load.cpu_tctl_max = 85.0;     // hot
    data.load.gpu_env_max = 70.0;
    ChannelStats cs;
    cs.setpoint_pct = {20.0, 22.0, 24.0, 26.0, 28.0};  // p90 < 35
    data.channels[0] = std::move(cs);

    const auto flags = BuildDiagnosticFlags(options, data);
    bool has = false;
    for (const auto& f : flags) {
        if (f == "hot_but_low_channel_setpoint") {
            has = true;
            break;
        }
    }
    ExpectTrue(has,
               "hot_but_low_channel_setpoint flag emitted when "
               "load above threshold and best channel p90 < 35%");
}

void TestBuildDiagnosticFlags_RobustnessAndSlowResponse() {
    ReportOptions options;
    options.load_threshold_c = 75.0;

    ReportData data;
    data.load.cpu_tctl_max = 60.0;  // not hot
    data.load.gpu_env_max = 60.0;
    data.onset_tick = 100;
    data.response_tick = std::nullopt;        // no response
    data.response_delay_s = std::nullopt;
    data.authority_reasserted = 1;
    data.write_failures = 2;
    data.restore_failures = 3;

    const auto flags = BuildDiagnosticFlags(options, data);

    bool no_response = false;
    bool auth = false;
    bool writes = false;
    bool restores = false;
    for (const auto& f : flags) {
        if (f == "no_setpoint_response_after_load_threshold") no_response = true;
        if (f == "authority_reasserted_during_run") auth = true;
        if (f == "write_failures_present") writes = true;
        if (f == "restore_failures_present") restores = true;
    }
    ExpectTrue(no_response,
               "no_setpoint_response_after_load_threshold flag emitted");
    ExpectTrue(auth, "authority_reasserted_during_run flag emitted");
    ExpectTrue(writes, "write_failures_present flag emitted");
    ExpectTrue(restores, "restore_failures_present flag emitted");
}

void TestBuildDiagnosticFlags_SlowResponse() {
    ReportOptions options;
    options.load_threshold_c = 75.0;

    ReportData data;
    data.onset_tick = 100;
    data.response_tick = 200;
    data.response_delay_s = 12.0;  // > 10 s threshold

    const auto flags = BuildDiagnosticFlags(options, data);
    bool slow = false;
    for (const auto& f : flags) {
        if (f == "slow_setpoint_response_after_load_threshold") {
            slow = true;
            break;
        }
    }
    ExpectTrue(slow,
               "slow_setpoint_response flag emitted when response_delay > 10s");
}

void TestComputePackagePower_EmptyIsUnavailable() {
    // No windows -> unavailable, NOT a false zero. acquisition provenance is
    // copied through so the report can still say why (RAPL off).
    PackagePowerSummary pp =
        ComputePackagePower({}, {{"disabled", 30}});
    ExpectEqualInt(pp.window_count, 0, "empty -> window_count 0");
    ExpectNullopt(pp.avg_watts, "empty -> avg_watts nullopt (no false zero)");
    ExpectNear(pp.total_energy_j, 0.0, kEps, "empty -> total_energy_j 0");
    ExpectNullopt(pp.watts_max, "empty -> watts_max nullopt");
    ExpectEqualInt(pp.acquisition_counts["disabled"], 30,
                   "empty -> acquisition provenance preserved");
}

void TestComputePackagePower_TimeWeighted() {
    // 10 J over 1 s (10 W), 30 J over 1 s (30 W), 20 J over 2 s (10 W).
    // delta is microjoules: 10 J = 1e7 uj.
    std::vector<PackageEnergyWindow> windows{
        {1000.0, 1.0e7},
        {1000.0, 3.0e7},
        {2000.0, 2.0e7},
    };
    PackagePowerSummary pp = ComputePackagePower(windows, {{"quarantine", 6}});
    ExpectEqualInt(pp.window_count, 3, "three distinct windows counted");
    ExpectNear(pp.total_energy_j, 60.0, 1e-6, "total energy 10+30+20 = 60 J");
    ExpectNear(pp.total_window_s, 4.0, 1e-6, "total window 1+1+2 = 4 s");
    // Time-weighted = 60 J / 4 s = 15 W, which is deliberately NOT the
    // mean-of-per-window-watts (10+30+10)/3 = 16.667 W.
    ExpectHasValue(pp.avg_watts, "avg_watts present for valid windows");
    ExpectNear(*pp.avg_watts, 15.0, 1e-6,
               "time-weighted avg = total energy / total window time");
    ExpectTrue(std::fabs(*pp.avg_watts - (10.0 + 30.0 + 10.0) / 3.0) > 1.0,
               "time-weighted avg differs from mean-of-means");
    // Per-window watts {10, 30, 10}: nearest-rank p50=10, p90=max=30.
    ExpectNear(*pp.watts_p50, 10.0, 1e-6, "per-window watts p50 == 10");
    ExpectNear(*pp.watts_p90, 30.0, 1e-6, "per-window watts p90 == 30");
    ExpectNear(*pp.watts_max, 30.0, 1e-6, "per-window watts max == 30");
}

void TestComputePackagePower_SingleWindow() {
    // 20 J over 2 s = 10 W.
    std::vector<PackageEnergyWindow> windows{{2000.0, 2.0e7}};
    PackagePowerSummary pp = ComputePackagePower(windows, {});
    ExpectEqualInt(pp.window_count, 1, "single window counted");
    ExpectHasValue(pp.avg_watts, "single window avg present");
    ExpectNear(*pp.avg_watts, 10.0, 1e-6, "single window 20 J / 2 s = 10 W");
    ExpectNear(*pp.watts_max, 10.0, 1e-6, "single window watts_max == 10");
}

void TestBuildDiagnosticFlags_NoFlagsOnHealthyRun() {
    ReportOptions options;
    options.load_threshold_c = 75.0;

    ReportData data;
    data.load.cpu_tctl_max = 60.0;
    data.load.gpu_env_max = 60.0;
    data.onset_tick = 100;
    data.response_tick = 110;
    data.response_delay_s = 2.0;
    ChannelStats cs;
    cs.setpoint_pct = {40.0, 50.0, 60.0};
    data.channels[0] = std::move(cs);

    const auto flags = BuildDiagnosticFlags(options, data);
    ExpectEqualInt(static_cast<int>(flags.size()), 0,
                   "Healthy run emits no diagnostic flags");
}

}  // namespace

int main() {
    TestPercentile_Empty();
    TestPercentile_Single();
    TestPercentile_NearestRank();
    TestPercentile_ThreeAry();
    TestPercentile_Unsorted();
    TestMedian_Matches50p();
    TestSummariseBand_Empty();
    TestSummariseBand_FiltersByBand();
    TestSummariseBand_MissingOptionals();
    TestComputeElapsedTime_ParsedAdvances();
    TestComputeElapsedTime_StaticTimestampSynthetic();
    TestComputeElapsedTime_UnparseableSynthetic();
    TestAssignBands_AllIdle();
    TestAssignBands_LoadThenCooldown();
    TestAssignBands_HotInIdleIgnored();
    TestBuildDiagnosticFlags_HotButLowSetpoint();
    TestBuildDiagnosticFlags_RobustnessAndSlowResponse();
    TestBuildDiagnosticFlags_SlowResponse();
    TestBuildDiagnosticFlags_NoFlagsOnHealthyRun();
    TestComputePackagePower_EmptyIsUnavailable();
    TestComputePackagePower_TimeWeighted();
    TestComputePackagePower_SingleWindow();
    if (g_failures > 0) {
        std::cerr << g_failures << " analyze_report helper test failure(s)\n";
        return 1;
    }
    std::cout << "analyze_report helper tests OK\n";
    return 0;
}
