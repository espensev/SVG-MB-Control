#include "control_status_writer.h"

#include <cstddef>

namespace svg_mb_control {

void BuildChannelLogStates(
    const std::vector<ChannelState>& channels,
    std::vector<RuntimeControlChannelLogState>& out) {
    // resize (not clear) so existing elements — and their response_source /
    // write_reason string buffers — are reused; std::string assignment keeps
    // capacity when the new value fits. Steady state has a fixed channel
    // count, so resize is a no-op after the first tick.
    out.resize(channels.size());
    for (std::size_t i = 0u; i < channels.size(); ++i) {
        const ChannelState& channel = channels[i];
        RuntimeControlChannelLogState& state = out[i];
        state.channel = channel.config.channel;
        state.observed_temp_c = channel.last_observed_temp_c;
        state.setpoint_pct = channel.last_setpoint_pct;
        state.feedforward_pct = channel.last_raw_demand_pct;
        state.thermal_pressure_boost_pct =
            channel.thermal_pressure_boost_pct;
        state.midband_pressure_boost_pct =
            channel.midband_pressure_boost_pct;
        state.gpu_airflow_boost_pct = channel.gpu_airflow_boost_pct;
        state.cpu_low_soak_boost_pct = channel.cpu_low_soak_boost_pct;
        state.low_band_stage_boost_pct =
            channel.low_band_stage_boost_pct;
        state.low_band_effective_boost_pct =
            channel.low_band_effective_boost_pct;
        state.low_band_debt = channel.low_band_debt_snapshot;
        state.low_band_signal = channel.low_band_signal_snapshot;
        state.low_band_stage_active = channel.low_band_stage_active;
        state.response_source = channel.last_response_source;
        state.write_reason = channel.last_write_reason;
        state.total_writes = channel.total_writes;
        state.write_active = channel.write_active;
        state.baseline_captured = channel.baseline_captured;
    }
}

}  // namespace svg_mb_control
