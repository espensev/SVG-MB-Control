#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace svg_mb_control::analyze {

class Statement;
struct ParsedChannelSample;

enum class TickChannelSampleColumn {
    ObservedTempC,
    SetpointPct,
    ThermalPressureBoostPct,
    MidbandPressureBoostPct,
    GpuAirflowBoostPct,
    CpuLowSoakBoostPct,
    PrimaryTempSource,
    ResponseSource,
    WriteReason,
    TotalWrites,
    WriteActive,
    BaselineCaptured,
    FeedforwardPct,
    CorrectionPct,
    LowBandStageBoostPct,
    LowBandEffectiveBoostPct,
};

struct TickChannelSampleColumnSpec {
    TickChannelSampleColumn id;
    const char* name;
    const char* sql_type;
};

constexpr std::size_t kTickChannelSampleColumnCount = 16u;

const std::array<TickChannelSampleColumnSpec,
                 kTickChannelSampleColumnCount>&
TickChannelSampleColumns();
const TickChannelSampleColumnSpec& TickChannelSampleColumnById(
    TickChannelSampleColumn id);

std::string TickChannelSampleCreateTableSql();
std::string TickChannelSampleInsertSql();
std::string TickChannelSampleSelectAllSql(std::string_view where_clause,
                                          std::string_view order_clause);
int TickChannelSampleSelectIndex(TickChannelSampleColumn id);
std::string TickChannelCsvFieldName(std::uint32_t channel,
                                    std::string_view suffix);

void BindTickChannelSample(Statement& stmt,
                           std::int64_t run_id,
                           std::int64_t tick_count,
                           const ParsedChannelSample& sample);

}  // namespace svg_mb_control::analyze
