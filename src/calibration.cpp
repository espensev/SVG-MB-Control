#include "calibration.h"

#include "amd_reader.h"
#include "channel_evaluator.h"
#include "control_scheduler.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace svg_mb_control {

CalibrationOptions DefaultCalibrationOptions() {
    CalibrationOptions options;
    options.sequence = {
        {20.0, 60000u},
        {35.0, 60000u},
        {55.0, 60000u},
        {75.0, 60000u},
        {90.0, 60000u},
        {20.0, 120000u},
    };
    options.settle_window_ms = 10000u;
    options.abort_temp_ceiling_c = 95.0;
    return options;
}

namespace {

constexpr std::uint32_t kPollIntervalMs = 200u;

double MaxAmdTemp(const RuntimeSnapshot& s) {
    double m = std::numeric_limits<double>::quiet_NaN();
    for (const auto& sensor : s.amd_sensors) {
        if (std::isnan(m) || sensor.temperature_c > m) {
            m = sensor.temperature_c;
        }
    }
    return m;
}

double Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

void AccumulateMeanStddev(const std::vector<double>& values,
                          double& mean, double& stddev) {
    if (values.empty()) {
        mean = std::numeric_limits<double>::quiet_NaN();
        stddev = std::numeric_limits<double>::quiet_NaN();
        return;
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    mean = sum / static_cast<double>(values.size());
    if (values.size() < 2u) {
        stddev = 0.0;
        return;
    }
    double accum = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        accum += d * d;
    }
    stddev = std::sqrt(accum / static_cast<double>(values.size() - 1u));
}

void EmitJsonNumber(nlohmann::json& obj,
                    const std::string& key,
                    double value) {
    obj[key] = std::isfinite(value)
        ? nlohmann::json(value)
        : nlohmann::json(nullptr);
}

struct StepObservation {
    double duty_pct_target = 0.0;
    double duty_pct_observed_mean = std::numeric_limits<double>::quiet_NaN();
    double rpm_mean = std::numeric_limits<double>::quiet_NaN();
    double rpm_stddev = std::numeric_limits<double>::quiet_NaN();
    double tctl_c_mean = std::numeric_limits<double>::quiet_NaN();
    double gpu_envelope_c_mean = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t hold_ms = 0u;
    std::uint32_t settle_window_ms = 0u;
    std::uint32_t settle_sample_count = 0u;
};

struct ChannelCalibration {
    std::uint32_t channel = 0u;
    bool baseline_captured = false;
    std::uint8_t baseline_duty_raw = 0u;
    std::uint8_t baseline_mode_raw = 0u;
    double baseline_rpm = std::numeric_limits<double>::quiet_NaN();
    double baseline_tctl_c = std::numeric_limits<double>::quiet_NaN();
    double baseline_gpu_envelope_c = std::numeric_limits<double>::quiet_NaN();
    std::vector<StepObservation> steps;
    bool restored = false;
    std::string note;
};

bool HoldAndSampleStep(AmdReader& amd_reader,
                       GpuReader& gpu_reader,
                       FanWriter& fan_writer,
                       const RuntimeWritePolicy& runtime_policy,
                       std::uint32_t channel,
                       const CalibrationStepSpec& step,
                       std::uint32_t settle_window_ms,
                       double abort_temp_ceiling_c,
                       const std::atomic<bool>& stop_flag,
                       StepObservation& observation,
                       std::string& abort_reason) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto deadline = start + std::chrono::milliseconds(step.hold_ms);
    const auto settle_start =
        deadline - std::chrono::milliseconds(settle_window_ms);
    std::vector<double> rpm_samples;
    std::vector<double> duty_samples;
    std::vector<double> tctl_samples;
    std::vector<double> gpu_envelope_samples;

    while (true) {
        if (stop_flag.load()) {
            abort_reason = "stop_signaled";
            return true;
        }
        const auto now = clock::now();
        if (now >= deadline) {
            break;
        }

        const RuntimeSnapshot snapshot = SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, fan_writer, runtime_policy);
        const double max_amd = MaxAmdTemp(snapshot);
        if (!std::isnan(max_amd) && max_amd > abort_temp_ceiling_c) {
            abort_reason = "amd_temp_ceiling_exceeded";
            return true;
        }

        if (now >= settle_start) {
            if (const RuntimeFanSnapshot* fan =
                    FindRuntimeFanChannel(snapshot, channel)) {
                rpm_samples.push_back(static_cast<double>(fan->rpm));
                duty_samples.push_back(fan->duty_percent);
            }
            const double tctl =
                FindRuntimeAmdSensorTemperature(snapshot, "Tctl/Tdie");
            if (!std::isnan(tctl)) {
                tctl_samples.push_back(tctl);
            }
            if (snapshot.gpu.available) {
                gpu_envelope_samples.push_back(
                    GpuControlEnvelopeC(snapshot.gpu));
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
    }

    AccumulateMeanStddev(rpm_samples,
                         observation.rpm_mean, observation.rpm_stddev);
    observation.duty_pct_observed_mean = Mean(duty_samples);
    observation.tctl_c_mean = Mean(tctl_samples);
    observation.gpu_envelope_c_mean = Mean(gpu_envelope_samples);
    observation.settle_sample_count =
        static_cast<std::uint32_t>(rpm_samples.size());
    return false;
}

}  // namespace

int RunCalibration(const ControlConfig& base,
                   const std::filesystem::path& runtime_home,
                   const CalibrationOptions& options,
                   const std::atomic<bool>& stop_flag) {
    if (options.sequence.empty()) {
        std::cerr << "Error: calibration sequence is empty.\n";
        return 1;
    }

    const RuntimeWritePolicy runtime_policy =
        ResolveRuntimeWritePolicy(&base);
    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(runtime_policy);
    } catch (const std::exception& error) {
        std::cerr << "Error: fan writer init failed: " << error.what() << '\n';
        return 1;
    }

    AmdReader amd_reader;
    GpuReader gpu_reader;

    const RuntimeSnapshot initial = SampleDirectRuntimeSnapshot(
        amd_reader, gpu_reader, *fan_writer, runtime_policy);

    std::vector<std::uint32_t> channels;
    for (const auto& fan : initial.fans) {
        if (options.only_channel.has_value() &&
            *options.only_channel != fan.channel) {
            continue;
        }
        if (fan.policy_blocked || !fan.write_allowed) {
            continue;
        }
        channels.push_back(fan.channel);
    }

    if (channels.empty()) {
        std::cerr
            << "Error: no writable channels available for calibration.\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);

    std::vector<ChannelCalibration> results;
    std::string abort_reason;

    for (const std::uint32_t channel : channels) {
        if (stop_flag.load()) {
            abort_reason = "stop_signaled";
            break;
        }
        ChannelCalibration entry;
        entry.channel = channel;

        const FanReadResult baseline = fan_writer->ReadChannelState(channel);
        if (!baseline.ok()) {
            entry.note = std::string("baseline_read_failed: ") +
                         baseline.detail;
            results.push_back(entry);
            continue;
        }
        entry.baseline_captured = true;
        entry.baseline_duty_raw = baseline.state.duty_raw;
        entry.baseline_mode_raw = baseline.state.mode_raw;
        entry.baseline_rpm = static_cast<double>(baseline.state.rpm);
        entry.baseline_tctl_c =
            FindRuntimeAmdSensorTemperature(initial, "Tctl/Tdie");
        if (initial.gpu.available) {
            entry.baseline_gpu_envelope_c = GpuControlEnvelopeC(initial.gpu);
        }

        PendingWriteEntry sidecar;
        sidecar.channel = channel;
        sidecar.baseline_duty_raw = entry.baseline_duty_raw;
        sidecar.baseline_mode_raw = entry.baseline_mode_raw;
        sidecar.target_pct = options.sequence.front().duty_pct;
        sidecar.requested_hold_ms = 0u;
        sidecar.child_pid = 0u;
        try {
            UpsertPendingWrite(runtime_home, sidecar);
        } catch (const std::exception& e) {
            entry.note = std::string("sidecar_upsert_failed: ") + e.what();
            results.push_back(entry);
            continue;
        }

        bool channel_aborted = false;
        for (const auto& step : options.sequence) {
            if (stop_flag.load()) {
                abort_reason = "stop_signaled";
                channel_aborted = true;
                break;
            }
            const FanWriteResult write_result =
                fan_writer->ApplyDuty(channel, step.duty_pct);
            if (!write_result.ok()) {
                abort_reason = std::string("apply_duty_failed: ") +
                               write_result.detail;
                channel_aborted = true;
                break;
            }
            StepObservation observation;
            observation.duty_pct_target = step.duty_pct;
            observation.hold_ms = step.hold_ms;
            observation.settle_window_ms = std::min(step.hold_ms,
                                                    options.settle_window_ms);
            std::string step_abort_reason;
            const bool step_aborted = HoldAndSampleStep(
                amd_reader, gpu_reader, *fan_writer, runtime_policy,
                channel, step, observation.settle_window_ms,
                options.abort_temp_ceiling_c, stop_flag, observation,
                step_abort_reason);
            entry.steps.push_back(observation);
            if (step_aborted) {
                abort_reason = step_abort_reason;
                channel_aborted = true;
                break;
            }
        }

        const FanWriteResult restore = fan_writer->RestoreSavedState(
            channel, entry.baseline_duty_raw, entry.baseline_mode_raw,
            base.restore_timeout_ms);
        entry.restored = restore.ok();
        if (!restore.ok() && entry.note.empty()) {
            entry.note = std::string("restore_failed: ") + restore.detail;
        }
        try {
            RemovePendingWrite(runtime_home, channel);
        } catch (const std::exception&) {
        }

        results.push_back(entry);
        if (channel_aborted) {
            break;
        }
    }

    nlohmann::json payload;
    payload["schema"] = "svg_mb_control.plant_model_capture.v1";
    payload["schema_version"] = 1;
    payload["captured_local"] =
        FormatLocalIso8601(std::chrono::system_clock::now());
    payload["abort_reason"] = abort_reason.empty()
        ? nlohmann::json(nullptr)
        : nlohmann::json(abort_reason);

    nlohmann::json producer;
    producer["tool"] = "svg-mb-control";
#ifdef SVG_MB_CONTROL_VERSION
    producer["version"] = SVG_MB_CONTROL_VERSION;
#endif
#ifdef SVG_MB_CONTROL_GIT_HASH
    producer["git_hash"] = SVG_MB_CONTROL_GIT_HASH;
#endif
    payload["producer"] = producer;

    nlohmann::json options_json;
    options_json["settle_window_ms"] = options.settle_window_ms;
    options_json["abort_temp_ceiling_c"] = options.abort_temp_ceiling_c;
    options_json["sequence"] = nlohmann::json::array();
    for (const auto& step : options.sequence) {
        options_json["sequence"].push_back({
            {"duty_pct", step.duty_pct},
            {"hold_ms", step.hold_ms},
        });
    }
    payload["options"] = options_json;

    payload["channels"] = nlohmann::json::array();
    for (const auto& entry : results) {
        nlohmann::json channel_json;
        channel_json["channel"] = entry.channel;
        channel_json["baseline_captured"] = entry.baseline_captured;
        channel_json["restored"] = entry.restored;
        if (!entry.note.empty()) {
            channel_json["note"] = entry.note;
        }
        nlohmann::json baseline_json;
        baseline_json["duty_raw"] = entry.baseline_duty_raw;
        baseline_json["mode_raw"] = entry.baseline_mode_raw;
        EmitJsonNumber(baseline_json, "rpm", entry.baseline_rpm);
        EmitJsonNumber(baseline_json, "tctl_c", entry.baseline_tctl_c);
        EmitJsonNumber(baseline_json, "gpu_envelope_c",
                       entry.baseline_gpu_envelope_c);
        channel_json["baseline"] = baseline_json;
        channel_json["steps"] = nlohmann::json::array();
        for (const auto& step : entry.steps) {
            nlohmann::json step_json;
            step_json["duty_pct_target"] = step.duty_pct_target;
            step_json["hold_ms"] = step.hold_ms;
            step_json["settle_window_ms"] = step.settle_window_ms;
            step_json["settle_sample_count"] = step.settle_sample_count;
            EmitJsonNumber(step_json, "duty_pct_observed_mean",
                           step.duty_pct_observed_mean);
            EmitJsonNumber(step_json, "rpm_mean", step.rpm_mean);
            EmitJsonNumber(step_json, "rpm_stddev", step.rpm_stddev);
            EmitJsonNumber(step_json, "tctl_c_mean", step.tctl_c_mean);
            EmitJsonNumber(step_json, "gpu_envelope_c_mean",
                           step.gpu_envelope_c_mean);
            channel_json["steps"].push_back(step_json);
        }
        payload["channels"].push_back(channel_json);
    }

    const std::filesystem::path output =
        options.output_path.empty()
            ? runtime_home / "plant_model.json"
            : options.output_path;
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), ec);
    }
    if (!TryWriteJsonFileAtomic(output, payload)) {
        std::cerr << "Error: failed to write " << output.string() << '\n';
        return 1;
    }
    std::cout << "calibration: wrote " << output.string()
              << " channels=" << results.size();
    if (!abort_reason.empty()) {
        std::cout << " aborted=" << abort_reason;
    }
    std::cout << '\n';
    return abort_reason.empty() ? 0 : 1;
}

}  // namespace svg_mb_control
