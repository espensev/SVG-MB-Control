#include "analyze_channel_sample_columns.h"

#include "analyze_csv.h"
#include "analyze_db.h"

#include <sstream>
#include <stdexcept>

namespace svg_mb_control::analyze {

namespace {

constexpr std::array<TickChannelSampleColumnSpec,
                     kTickChannelSampleColumnCount>
    kTickChannelSampleColumns{{
        {TickChannelSampleColumn::ObservedTempC,
         "observed_temp_c", "REAL", TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::SetpointPct,
         "setpoint_pct", "REAL", TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::ThermalPressureBoostPct,
         "thermal_pressure_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::MidbandPressureBoostPct,
         "midband_pressure_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::GpuAirflowBoostPct,
         "gpu_airflow_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::CpuLowSoakBoostPct,
         "cpu_low_soak_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::PrimaryTempSource,
         "primary_temp_source", "TEXT", TickChannelSampleValueKind::Text},
        {TickChannelSampleColumn::ResponseSource,
         "response_source", "TEXT", TickChannelSampleValueKind::Text},
        {TickChannelSampleColumn::WriteReason,
         "write_reason", "TEXT", TickChannelSampleValueKind::Text},
        {TickChannelSampleColumn::TotalWrites,
         "total_writes", "INTEGER", TickChannelSampleValueKind::Integer},
        {TickChannelSampleColumn::WriteActive,
         "write_active", "INTEGER", TickChannelSampleValueKind::Integer},
        {TickChannelSampleColumn::BaselineCaptured,
         "baseline_captured", "INTEGER", TickChannelSampleValueKind::Integer},
        {TickChannelSampleColumn::FeedforwardPct,
         "feedforward_pct", "REAL", TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::CorrectionPct,
         "correction_pct", "REAL", TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::LowBandStageBoostPct,
         "low_band_stage_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
        {TickChannelSampleColumn::LowBandEffectiveBoostPct,
         "low_band_effective_boost_pct", "REAL",
         TickChannelSampleValueKind::Real},
    }};

std::size_t FindColumnOffset(TickChannelSampleColumn id) {
    for (std::size_t i = 0; i < kTickChannelSampleColumns.size(); ++i) {
        if (kTickChannelSampleColumns[i].id == id) {
            return i;
        }
    }
    throw std::logic_error("unknown tick_channel_samples column id");
}

void AppendDescriptorColumnList(std::ostringstream& out) {
    for (const auto& spec : kTickChannelSampleColumns) {
        out << ", " << spec.name;
    }
}

void AppendPlaceholders(std::ostringstream& out, int count) {
    for (int i = 1; i <= count; ++i) {
        if (i > 1) {
            out << ',';
        }
        out << '?' << i;
    }
}

void BindDescriptorValue(Statement& stmt,
                         int index,
                         TickChannelSampleColumn id,
                         const ParsedChannelSample& sample) {
    switch (id) {
        case TickChannelSampleColumn::ObservedTempC:
            stmt.BindOptionalDouble(index, sample.observed_temp_c);
            break;
        case TickChannelSampleColumn::SetpointPct:
            stmt.BindOptionalDouble(index, sample.setpoint_pct);
            break;
        case TickChannelSampleColumn::ThermalPressureBoostPct:
            stmt.BindOptionalDouble(index, sample.thermal_pressure_boost_pct);
            break;
        case TickChannelSampleColumn::MidbandPressureBoostPct:
            stmt.BindOptionalDouble(index, sample.midband_pressure_boost_pct);
            break;
        case TickChannelSampleColumn::GpuAirflowBoostPct:
            stmt.BindOptionalDouble(index, sample.gpu_airflow_boost_pct);
            break;
        case TickChannelSampleColumn::CpuLowSoakBoostPct:
            stmt.BindOptionalDouble(index, sample.cpu_low_soak_boost_pct);
            break;
        case TickChannelSampleColumn::PrimaryTempSource:
            stmt.BindOptionalText(index, sample.primary_temp_source);
            break;
        case TickChannelSampleColumn::ResponseSource:
            stmt.BindOptionalText(index, sample.response_source);
            break;
        case TickChannelSampleColumn::WriteReason:
            stmt.BindOptionalText(index, sample.write_reason);
            break;
        case TickChannelSampleColumn::TotalWrites:
            stmt.BindOptionalInt(index, sample.total_writes);
            break;
        case TickChannelSampleColumn::WriteActive:
            stmt.BindOptionalInt(index, sample.write_active);
            break;
        case TickChannelSampleColumn::BaselineCaptured:
            stmt.BindOptionalInt(index, sample.baseline_captured);
            break;
        case TickChannelSampleColumn::FeedforwardPct:
            stmt.BindOptionalDouble(index, sample.feedforward_pct);
            break;
        case TickChannelSampleColumn::CorrectionPct:
            stmt.BindOptionalDouble(index, sample.correction_pct);
            break;
        case TickChannelSampleColumn::LowBandStageBoostPct:
            stmt.BindOptionalDouble(index, sample.low_band_stage_boost_pct);
            break;
        case TickChannelSampleColumn::LowBandEffectiveBoostPct:
            stmt.BindOptionalDouble(index, sample.low_band_effective_boost_pct);
            break;
    }
}

}  // namespace

const std::array<TickChannelSampleColumnSpec,
                 kTickChannelSampleColumnCount>&
TickChannelSampleColumns() {
    return kTickChannelSampleColumns;
}

const TickChannelSampleColumnSpec& TickChannelSampleColumnById(
    TickChannelSampleColumn id) {
    return kTickChannelSampleColumns[FindColumnOffset(id)];
}

std::string TickChannelSampleCreateTableSql() {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS tick_channel_samples (\n"
        << "    run_id INTEGER NOT NULL,\n"
        << "    tick_count INTEGER NOT NULL,\n"
        << "    channel INTEGER NOT NULL,\n";
    for (const auto& spec : kTickChannelSampleColumns) {
        sql << "    " << spec.name << ' ' << spec.sql_type << ",\n";
    }
    sql << "    PRIMARY KEY (run_id, tick_count, channel),\n"
        << "    FOREIGN KEY (run_id, tick_count)\n"
        << "        REFERENCES tick_samples(run_id, tick_count) "
           "ON DELETE CASCADE\n"
        << ")";
    return sql.str();
}

std::string TickChannelSampleInsertSql() {
    std::ostringstream sql;
    sql << "INSERT INTO tick_channel_samples("
        << "run_id, tick_count, channel";
    AppendDescriptorColumnList(sql);
    sql << ") VALUES (";
    AppendPlaceholders(
        sql, 3 + static_cast<int>(kTickChannelSampleColumns.size()));
    sql << ")";
    return sql.str();
}

std::string TickChannelSampleSelectAllSql(std::string_view where_clause,
                                          std::string_view order_clause) {
    std::ostringstream sql;
    sql << "SELECT tick_count, channel";
    AppendDescriptorColumnList(sql);
    sql << " FROM tick_channel_samples";
    if (!where_clause.empty()) {
        sql << ' ' << where_clause;
    }
    if (!order_clause.empty()) {
        sql << ' ' << order_clause;
    }
    return sql.str();
}

int TickChannelSampleSelectIndex(TickChannelSampleColumn id) {
    return 2 + static_cast<int>(FindColumnOffset(id));
}

std::string TickChannelCsvFieldName(std::uint32_t channel,
                                    std::string_view suffix) {
    std::string name = "channel";
    name += std::to_string(channel);
    name += '_';
    name.append(suffix.data(), suffix.size());
    return name;
}

void BindTickChannelSample(Statement& stmt,
                           std::int64_t run_id,
                           std::int64_t tick_count,
                           const ParsedChannelSample& sample) {
    stmt.BindInt(1, run_id);
    stmt.BindInt(2, tick_count);
    stmt.BindInt(3, sample.channel);
    int index = 4;
    for (const auto& spec : kTickChannelSampleColumns) {
        BindDescriptorValue(stmt, index, spec.id, sample);
        ++index;
    }
}

}  // namespace svg_mb_control::analyze
