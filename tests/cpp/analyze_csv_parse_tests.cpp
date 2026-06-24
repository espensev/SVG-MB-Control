// Unit tests for the analyze_csv row parser and its precomputed column->index
// plan (BuildTickRowParsePlan / ParseTickRow). The plan overload and the
// one-shot convenience overload (which builds a plan internally) must agree,
// and the hand-written exact-value, absent-column, ragged-row, true/false,
// repeated fan/channel-block, and gpu_envelope fallback assertions pin the
// concrete parsed values so a future refactor of the hot parse loop regresses
// loudly. ParseCsvLine's quote/escape branch is exercised directly.
//
// The DB-bound ingest path is covered by the Python hermetic suite
// (tests/test_analyze_ingest.py); here we exercise only the in-memory parser.

#include "analyze/analyze_csv.h"

#include "test_helpers.h"

#include "analyze/analyze_channel_sample_columns.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using svg_mb_control::analyze::BuildTickRowParsePlan;
using svg_mb_control::analyze::CsvHeader;
using svg_mb_control::analyze::ParseCsvHeader;
using svg_mb_control::analyze::ParseCsvLine;
using svg_mb_control::analyze::ParsedChannelSample;
using svg_mb_control::analyze::ParsedFanSample;
using svg_mb_control::analyze::ParsedTickRow;
using svg_mb_control::analyze::ParseTickRow;
using svg_mb_control::analyze::TickChannelCsvFieldName;
using svg_mb_control::analyze::TickChannelSampleColumns;
using svg_mb_control::analyze::TickRowParsePlan;

// Joins fields into one CSV header/data line. Values are emitted verbatim;
// quoting/escaping is exercised separately by TestParseCsvLineQuoting below.
std::string JoinCsv(const std::vector<std::string>& fields) {
	std::string line;
	for (std::size_t i = 0; i < fields.size(); ++i) {
		if (i > 0) {
			line += ',';
		}
		line += fields[i];
	}
	return line;
}

// Builds a representative control-loop header column list: a handful of the
// fixed columns, two fan blocks, and two channel blocks (channel blocks use the
// real descriptor names so the test stays in sync with the schema).
std::vector<std::string> SampleColumns() {
	std::vector<std::string> cols = {
		"wall_clock",      "loop_tick_count", "mode",
		"cpu_tctl_c",      "cpu_max_c",       "gpu_core_c",
		"gpu_memjn_c",     "gpu_hotspot_c",   "gpu_envelope_c",
		"fan_count",       "loop_overrun",    "cadence_transient",
	};
	const char* const fan_suffixes[] = {
		"present", "label",          "rpm",           "tach_raw",
		"tach_valid", "duty_raw",     "duty_pct",      "mode_raw",
		"manual_override", "write_allowed", "policy_blocked",
		"effective_write_allowed",
	};
	for (std::uint32_t fi = 0u; fi < 2u; ++fi) {
		const std::string prefix = "fan" + std::to_string(fi) + "_";
		for (const char* suffix : fan_suffixes) {
			cols.push_back(prefix + suffix);
		}
	}
	for (std::uint32_t ci = 0u; ci < 2u; ++ci) {
		for (const auto& spec : TickChannelSampleColumns()) {
			cols.push_back(TickChannelCsvFieldName(ci, spec.name));
		}
	}
	return cols;
}

void ExpectOptDouble(const std::optional<double>& actual,
					 const std::optional<double>& expected,
					 const std::string& message) {
	if (actual.has_value() != expected.has_value()) {
		++g_failures;
		std::cerr << "FAIL: " << message << " has_value mismatch\n";
		return;
	}
	if (actual.has_value()) {
		ExpectNear(*actual, *expected, 1e-9, message);
	}
}

void ExpectOptInt(const std::optional<std::int64_t>& actual,
				  const std::optional<std::int64_t>& expected,
				  const std::string& message) {
	if (actual.has_value() != expected.has_value()) {
		++g_failures;
		std::cerr << "FAIL: " << message << " has_value mismatch\n";
		return;
	}
	if (actual.has_value() && *actual != *expected) {
		++g_failures;
		std::cerr << "FAIL: " << message << " expected " << *expected
				  << " got " << *actual << '\n';
	}
}

void ExpectOptText(const std::optional<std::string>& actual,
				   const std::optional<std::string>& expected,
				   const std::string& message) {
	if (actual.has_value() != expected.has_value()) {
		++g_failures;
		std::cerr << "FAIL: " << message << " has_value mismatch\n";
		return;
	}
	if (actual.has_value()) {
		ExpectEqual(*actual, *expected, message);
	}
}

// Deep field-by-field comparison of two parsed rows. Used to prove the plan
// overload and the one-shot convenience overload agree exactly.
void ExpectRowsEqual(const ParsedTickRow& a, const ParsedTickRow& b,
					 const std::string& ctx) {
	ExpectTrue(a.tick_count == b.tick_count, ctx + ": tick_count");
	ExpectEqual(a.wall_clock, b.wall_clock, ctx + ": wall_clock");
	ExpectOptText(a.mode, b.mode, ctx + ": mode");
	ExpectOptDouble(a.cpu_tctl_c, b.cpu_tctl_c, ctx + ": cpu_tctl_c");
	ExpectOptDouble(a.cpu_max_c, b.cpu_max_c, ctx + ": cpu_max_c");
	ExpectOptDouble(a.gpu_envelope_c, b.gpu_envelope_c, ctx + ": gpu_envelope");
	ExpectOptInt(a.fan_count, b.fan_count, ctx + ": fan_count");
	ExpectOptInt(a.loop_overrun, b.loop_overrun, ctx + ": loop_overrun");
	ExpectOptDouble(a.cadence_transient, b.cadence_transient,
					ctx + ": cadence_transient");

	ExpectTrue(a.fans.size() == b.fans.size(), ctx + ": fan count");
	for (std::size_t i = 0; i < a.fans.size() && i < b.fans.size(); ++i) {
		const std::string fc = ctx + ": fan" + std::to_string(i);
		ExpectTrue(a.fans[i].fan_index == b.fans[i].fan_index, fc + " index");
		ExpectOptInt(a.fans[i].present, b.fans[i].present, fc + " present");
		ExpectOptText(a.fans[i].label, b.fans[i].label, fc + " label");
		ExpectOptInt(a.fans[i].rpm, b.fans[i].rpm, fc + " rpm");
		ExpectOptDouble(a.fans[i].duty_pct, b.fans[i].duty_pct, fc + " duty");
		ExpectOptInt(a.fans[i].effective_write_allowed,
					 b.fans[i].effective_write_allowed, fc + " eff_write");
	}

	ExpectTrue(a.channels.size() == b.channels.size(), ctx + ": chan count");
	for (std::size_t i = 0; i < a.channels.size() && i < b.channels.size();
		 ++i) {
		const std::string cc = ctx + ": chan" + std::to_string(i);
		ExpectTrue(a.channels[i].channel == b.channels[i].channel,
				   cc + " channel");
		ExpectOptDouble(a.channels[i].observed_temp_c,
						b.channels[i].observed_temp_c, cc + " observed");
		ExpectOptDouble(a.channels[i].setpoint_pct,
						b.channels[i].setpoint_pct, cc + " setpoint");
		ExpectOptText(a.channels[i].primary_temp_source,
					  b.channels[i].primary_temp_source, cc + " primary_src");
		ExpectOptInt(a.channels[i].total_writes, b.channels[i].total_writes,
					 cc + " total_writes");
		ExpectOptDouble(a.channels[i].low_band_effective_boost_pct,
						b.channels[i].low_band_effective_boost_pct,
						cc + " low_band_eff");
	}
}

// Builds a data row whose value for each column equals a deterministic, type-
// appropriate token, so we can assert exact parsed values.
std::string BuildFullDataRow(const std::vector<std::string>& cols) {
	std::vector<std::string> values;
	values.reserve(cols.size());
	for (const std::string& name : cols) {
		if (name == "wall_clock") {
			values.push_back("2026-06-20T10:00:00Z");
		} else if (name == "loop_tick_count") {
			values.push_back("42");
		} else if (name == "mode") {
			values.push_back("control-loop");
		} else if (name.rfind("fan", 0) == 0 &&
				   name.find("_label") != std::string::npos) {
			values.push_back("Fan-X");
		} else if (name.find("primary_temp_source") != std::string::npos ||
				   name.find("response_source") != std::string::npos ||
				   name.find("write_reason") != std::string::npos) {
			values.push_back("src");
		} else if (name.find("_present") != std::string::npos ||
				   name.find("tach_valid") != std::string::npos ||
				   name.find("manual_override") != std::string::npos ||
				   name.find("write_allowed") != std::string::npos ||
				   name.find("policy_blocked") != std::string::npos ||
				   name.find("baseline_captured") != std::string::npos ||
				   name.find("write_active") != std::string::npos) {
			values.push_back("true");  // bool-as-int column
		} else if (name == "gpu_core_c" || name == "gpu_memjn_c" ||
				   name == "gpu_hotspot_c" || name == "gpu_envelope_c" ||
				   name == "cpu_tctl_c" || name == "cpu_max_c" ||
				   name == "cadence_transient" ||
				   name.find("duty_pct") != std::string::npos ||
				   name.find("_pct") != std::string::npos ||
				   name.find("temp_c") != std::string::npos) {
			values.push_back("12.5");  // double column
		} else {
			values.push_back("7");  // generic integer column
		}
	}
	return JoinCsv(values);
}

// The headline test: plan overload == one-shot convenience overload, for a
// fully populated row. Both build the same plan, so this guards future
// divergence of the two overloads; the exact-value tests below pin real values.
void TestPlanMatchesOneShotOverload_FullRow() {
	const std::vector<std::string> cols = SampleColumns();
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);
	const std::string line = BuildFullDataRow(cols);

	const auto via_plan = ParseTickRow(header, plan, line);
	const auto via_oneshot = ParseTickRow(header, line);
	ExpectTrue(via_plan.has_value(), "plan path parses full row");
	ExpectTrue(via_oneshot.has_value(), "one-shot path parses full row");
	if (via_plan.has_value() && via_oneshot.has_value()) {
		ExpectRowsEqual(*via_plan, *via_oneshot, "full row plan-vs-oneshot");
	}
}

// Exact parsed values for the full row: confirms true/false->int, doubles,
// fan/channel block population.
void TestFullRow_ExactValues() {
	const std::vector<std::string> cols = SampleColumns();
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);
	const auto row = ParseTickRow(header, plan, BuildFullDataRow(cols));
	ExpectTrue(row.has_value(), "full row parses");
	if (!row.has_value()) {
		return;
	}
	ExpectTrue(row->tick_count == 42, "tick_count parsed");
	ExpectEqual(row->wall_clock, "2026-06-20T10:00:00Z", "wall_clock parsed");
	ExpectOptText(row->mode, std::optional<std::string>("control-loop"),
				  "mode parsed");
	ExpectOptDouble(row->cpu_tctl_c, std::optional<double>(12.5),
					"cpu_tctl_c parsed");
	// Two fan blocks present.
	ExpectTrue(row->fans.size() == 2u, "two fan blocks parsed");
	if (row->fans.size() == 2u) {
		ExpectTrue(row->fans[0].fan_index == 0u, "fan0 index");
		ExpectTrue(row->fans[1].fan_index == 1u, "fan1 index");
		ExpectOptInt(row->fans[0].present, std::optional<std::int64_t>(1),
					 "fan0 present true->1");
		ExpectOptText(row->fans[0].label,
					  std::optional<std::string>("Fan-X"), "fan0 label");
	}
	// Two channel blocks present.
	ExpectTrue(row->channels.size() == 2u, "two channel blocks parsed");
	if (row->channels.size() == 2u) {
		ExpectTrue(row->channels[1].channel == 1u, "channel1 index");
		ExpectOptDouble(row->channels[0].observed_temp_c,
						std::optional<double>(12.5), "channel0 observed_temp");
	}
}

// A column missing from the header parses as nullopt (not a crash, not zero).
void TestMissingColumn_IsNullopt() {
	std::vector<std::string> cols = SampleColumns();
	// Drop cpu_max_c entirely.
	std::vector<std::string> trimmed;
	for (const std::string& c : cols) {
		if (c != "cpu_max_c") {
			trimmed.push_back(c);
		}
	}
	const CsvHeader header = ParseCsvHeader(JoinCsv(trimmed));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);
	const auto row = ParseTickRow(header, plan, BuildFullDataRow(trimmed));
	ExpectTrue(row.has_value(), "row without cpu_max_c still parses");
	if (row.has_value()) {
		ExpectFalse(row->cpu_max_c.has_value(),
					"absent cpu_max_c column -> nullopt");
		ExpectTrue(row->cpu_tctl_c.has_value(),
				   "present cpu_tctl_c still parsed");
	}
}

// A short/ragged data row (fewer fields than the header) yields nullopt for the
// trailing missing fields (the parser iterates header columns, not row fields).
void TestRaggedRow_TrailingNullopt() {
	const std::vector<std::string> cols = SampleColumns();
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);
	// Emit only the first 5 values (wall_clock, tick, mode, cpu_tctl_c,
	// cpu_max_c); everything after is absent in the row.
	std::vector<std::string> values = {
		"2026-06-20T10:00:00Z", "5", "control-loop", "30.0", "31.0",
	};
	const std::string line = JoinCsv(values);

	const auto via_plan = ParseTickRow(header, plan, line);
	const auto via_oneshot = ParseTickRow(header, line);
	ExpectTrue(via_plan.has_value(), "ragged row parses (plan)");
	ExpectTrue(via_oneshot.has_value(), "ragged row parses (one-shot)");
	if (via_plan.has_value()) {
		ExpectTrue(via_plan->tick_count == 5, "ragged tick parsed");
		ExpectFalse(via_plan->cadence_transient.has_value(),
					"trailing cadence_transient -> nullopt");
		// Fan/channel block COUNT follows the header layout, not the row's
		// field count: the header still declares both blocks, so two fan and
		// two channel samples are produced -- each with all-nullopt fields
		// because those columns are past the ragged row's end. (This matches
		// header-block iteration, not row-field count.)
		ExpectTrue(via_plan->fans.size() == 2u,
				   "ragged row keeps header fan-block count");
		ExpectTrue(via_plan->channels.size() == 2u,
				   "ragged row keeps header channel-block count");
		if (via_plan->fans.size() == 2u) {
			ExpectFalse(via_plan->fans[0].present.has_value(),
						"ragged fan0 present -> nullopt");
			ExpectFalse(via_plan->fans[1].label.has_value(),
						"ragged fan1 label -> nullopt");
		}
		if (via_plan->channels.size() == 2u) {
			ExpectFalse(via_plan->channels[0].observed_temp_c.has_value(),
						"ragged channel0 observed_temp -> nullopt");
		}
	}
	if (via_plan.has_value() && via_oneshot.has_value()) {
		ExpectRowsEqual(*via_plan, *via_oneshot, "ragged plan-vs-oneshot");
	}
}

// gpu_envelope_c blank in the row -> derived from core/memjn/hotspot, matching
// GpuEnvelopeC.
void TestGpuEnvelopeFallback() {
	const std::vector<std::string> cols = SampleColumns();
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);

	std::vector<std::string> values;
	values.reserve(cols.size());
	for (const std::string& name : cols) {
		if (name == "wall_clock") {
			values.push_back("2026-06-20T10:00:00Z");
		} else if (name == "loop_tick_count") {
			values.push_back("9");
		} else if (name == "gpu_core_c") {
			values.push_back("40.0");
		} else if (name == "gpu_memjn_c") {
			values.push_back("66.0");
		} else if (name == "gpu_hotspot_c") {
			values.push_back("55.0");
		} else if (name == "gpu_envelope_c") {
			values.push_back("");  // blank -> fallback
		} else {
			values.push_back("");
		}
	}
	const auto row = ParseTickRow(header, plan, JoinCsv(values));
	ExpectTrue(row.has_value(), "envelope-fallback row parses");
	if (row.has_value()) {
		ExpectOptDouble(row->gpu_envelope_c, std::optional<double>(66.0),
						"gpu_envelope derived as max(core,memjn,hotspot)");
	}
}

// The fan/channel block loops stop at the first absent block: if only fan0's
// block exists in the header, exactly one fan sample is produced.
void TestBlockRunStopsAtFirstGap() {
	// Header with fan0 block but no fan1 block, and channel0 block but no
	// channel1 block.
	std::vector<std::string> cols = {
		"wall_clock", "loop_tick_count",
	};
	const char* const fan_suffixes[] = {
		"present", "label",          "rpm",           "tach_raw",
		"tach_valid", "duty_raw",     "duty_pct",      "mode_raw",
		"manual_override", "write_allowed", "policy_blocked",
		"effective_write_allowed",
	};
	for (const char* suffix : fan_suffixes) {
		cols.push_back(std::string("fan0_") + suffix);
	}
	for (const auto& spec : TickChannelSampleColumns()) {
		cols.push_back(TickChannelCsvFieldName(0u, spec.name));
	}
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);

	ExpectTrue(plan.fans.size() == 1u, "plan records exactly one fan block");
	ExpectTrue(plan.channels.size() == 1u,
			   "plan records exactly one channel block");

	std::vector<std::string> values(cols.size(), "1");
	values[0] = "2026-06-20T10:00:00Z";  // wall_clock
	values[1] = "3";                      // loop_tick_count
	const auto row = ParseTickRow(header, plan, JoinCsv(values));
	ExpectTrue(row.has_value(), "single-block row parses");
	if (row.has_value()) {
		ExpectTrue(row->fans.size() == 1u, "exactly one fan sample");
		ExpectTrue(row->channels.size() == 1u, "exactly one channel sample");
	}
}

// Missing wall_clock or unparseable tick yields nullopt (row rejected).
void TestRowRejectedWithoutKeys() {
	const std::vector<std::string> cols = SampleColumns();
	const CsvHeader header = ParseCsvHeader(JoinCsv(cols));
	const TickRowParsePlan plan = BuildTickRowParsePlan(header);

	// Blank wall_clock.
	std::vector<std::string> v1(cols.size(), "");
	v1[1] = "10";  // tick present but wall_clock blank
	ExpectFalse(ParseTickRow(header, plan, JoinCsv(v1)).has_value(),
				"blank wall_clock rejects row");

	// Non-numeric tick.
	std::vector<std::string> v2(cols.size(), "");
	v2[0] = "2026-06-20T10:00:00Z";
	v2[1] = "not-a-number";
	ExpectFalse(ParseTickRow(header, plan, JoinCsv(v2)).has_value(),
				"non-numeric loop_tick_count rejects row");
}

// Directly exercises ParseCsvLine's RFC 4180-ish quote/escape branch (embedded
// commas inside quotes, "" -> a literal quote, a quoted-empty field, and a
// trailing empty field). The row-level tests never reach this branch because
// JoinCsv emits values verbatim; ParseCsvLine is byte-identical across this
// refactor, so these pin the read-path quoting an analyzer-CSV edit could
// otherwise regress silently.
void TestParseCsvLineQuoting() {
	const auto embedded = ParseCsvLine("a,\"b,c\",d");
	ExpectTrue(embedded.size() == 3u, "quoted comma keeps three fields");
	if (embedded.size() == 3u) {
		ExpectEqual(embedded[0], "a", "field before quoted comma");
		ExpectEqual(embedded[1], "b,c", "embedded comma preserved in quotes");
		ExpectEqual(embedded[2], "d", "field after quoted comma");
	}

	const auto escaped = ParseCsvLine("\"a\"\"b\",c");
	ExpectTrue(escaped.size() == 2u, "escaped-quote row keeps two fields");
	if (escaped.size() == 2u) {
		ExpectEqual(escaped[0], "a\"b", "doubled quote -> literal quote");
		ExpectEqual(escaped[1], "c", "field after escaped quote");
	}

	const auto quoted_empty = ParseCsvLine("\"\",x");
	ExpectTrue(quoted_empty.size() == 2u, "quoted-empty row keeps two fields");
	if (quoted_empty.size() == 2u) {
		ExpectEqual(quoted_empty[0], "", "quoted empty field is empty");
		ExpectEqual(quoted_empty[1], "x", "field after quoted empty");
	}

	const auto trailing_empty = ParseCsvLine("a,b,");
	ExpectTrue(trailing_empty.size() == 3u, "trailing comma yields empty field");
	if (trailing_empty.size() == 3u) {
		ExpectEqual(trailing_empty[2], "", "trailing field is empty");
	}
}

}  // namespace

int main() {
	TestParseCsvLineQuoting();
	TestPlanMatchesOneShotOverload_FullRow();
	TestFullRow_ExactValues();
	TestMissingColumn_IsNullopt();
	TestRaggedRow_TrailingNullopt();
	TestGpuEnvelopeFallback();
	TestBlockRunStopsAtFirstGap();
	TestRowRejectedWithoutKeys();
	if (g_failures > 0) {
		std::cerr << g_failures << " analyze_csv parse test failure(s)\n";
		return 1;
	}
	std::cout << "analyze_csv parse tests OK\n";
	return 0;
}
