#include "evidence_log.h"

#include "amd_reader.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "runtime_lifecycle.h"
#include "runtime_logging.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace svg_mb_control {

namespace {

std::uint32_t ResolveEvidenceStalenessThresholdMs(
    const ControlConfig& config) {
    if (config.staleness_threshold_ms > 0u) {
        return config.staleness_threshold_ms;
    }
    const std::uint32_t poll = config.poll_ms > 0u ? config.poll_ms : 1000u;
    return poll * 3u;
}

std::vector<RuntimeSioVoltageLogState> ConvertVoltages(
    const SioVoltageScanResult& result) {
    std::vector<RuntimeSioVoltageLogState> voltages;
    if (!result) {
        return voltages;
    }
    voltages.reserve(result.voltages.size());
    for (const auto& source : result.voltages) {
        voltages.push_back(RuntimeSioVoltageLogState{
            .index = source.index,
            .voltage_v = source.voltage_v,
            .raw = source.raw,
            .label = source.label,
        });
    }
    return voltages;
}

std::vector<RuntimeFanTachEvidenceLogState> ConvertFanTachEvidence(
    const FanTachEvidenceScanResult& result) {
    std::vector<RuntimeFanTachEvidenceLogState> fans;
    if (!result) {
        return fans;
    }
    fans.reserve(result.fans.size());
    for (const auto& source : result.fans) {
        fans.push_back(RuntimeFanTachEvidenceLogState{
            .channel = source.channel,
            .tach_hi_raw = source.tach_hi_raw,
            .tach_lo_raw = source.tach_lo_raw,
        });
    }
    return fans;
}

std::vector<RuntimeSioTemperatureLogState> ConvertTemperatures(
    const SioTemperatureScanResult& result) {
    std::vector<RuntimeSioTemperatureLogState> temperatures;
    if (!result) {
        return temperatures;
    }
    temperatures.reserve(result.temperatures.size());
    for (const auto& source : result.temperatures) {
        temperatures.push_back(RuntimeSioTemperatureLogState{
            .index = source.index,
            .temperature_c = source.temperature_c,
            .raw = source.raw,
            .half_raw = source.half_raw,
            .valid = source.valid,
            .label = source.label,
        });
    }
    return temperatures;
}

std::string BuildSioEvidenceDetail(
    const FanTachEvidenceScanResult& fan_tach_result,
    const SioVoltageScanResult& voltage_result,
    const SioTemperatureScanResult& temperature_result) {
    if (fan_tach_result && voltage_result && temperature_result) {
        return "SIO fan tach, voltage, and temperature evidence captured";
    }
    std::ostringstream detail;
    if (!fan_tach_result) {
        detail << "fan tach: " << fan_tach_result.detail;
    }
    if (!voltage_result) {
        if (detail.tellp() > 0) {
            detail << "; ";
        }
        detail << "voltage: " << voltage_result.detail;
    }
    if (!temperature_result) {
        if (detail.tellp() > 0) {
            detail << "; ";
        }
        detail << "temperature: " << temperature_result.detail;
    }
    return detail.str();
}

void WaitForNextEvidencePoll(const std::filesystem::path& runtime_home,
                             const std::atomic<bool>& stop_requested,
                             std::uint32_t poll_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(poll_ms);
    while (!stop_requested.load() &&
           !RuntimeStopRequested(runtime_home) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

}  // namespace

RuntimeArtifactNaming EvidenceLogArtifactNaming() {
    RuntimeArtifactNaming naming;
    naming.archive_prefix = "svg_mb_control_evidence";
    naming.latest_csv_name = "svg_mb_control_evidence.csv";
    naming.latest_events_name = "svg_mb_control_evidence_events.jsonl";
    naming.latest_manifest_name = "svg_mb_control_evidence_manifest.json";
    return naming;
}

int RunEvidenceLog(const ControlConfig& config,
                   const std::filesystem::path& runtime_home,
                   const std::atomic<bool>& stop_requested) {
    const std::uint32_t poll_ms = config.poll_ms > 0u ? config.poll_ms : 1000u;
    const std::uint32_t staleness_threshold_ms =
        ResolveEvidenceStalenessThresholdMs(config);
    const RuntimeArtifactNaming naming = EvidenceLogArtifactNaming();

    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    ClearRuntimeStopRequest(runtime_home);

    RuntimeCsvLogger csv_logger(runtime_home,
                                 config.log_rotate_hours,
                                 config.log_retain_days,
                                 config.csv_flush_interval_rows,
                                 naming);
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(runtime_home, naming).string();
    if (!csv_logger.Open("evidence-log", BuildEvidenceLogCsvHeader())) {
        AppendRuntimeEvent(
            runtime_home,
            RuntimeLogEvent{
                .mode = "evidence-log",
                .event_type = "evidence_log.init_failed",
                .detail = "evidence CSV logger could not open",
                .success = false,
                .event_log_path = event_log_path,
            },
            naming);
        return 1;
    }

    const std::string log_csv_path = csv_logger.active_archive_path().string();
    const RuntimeWritePolicy runtime_policy = ResolveRuntimeWritePolicy(&config);

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(runtime_policy);
    } catch (const std::exception& error) {
        AppendRuntimeEvent(
            runtime_home,
            RuntimeLogEvent{
                .mode = "evidence-log",
                .event_type = "evidence_log.init_failed",
                .detail = std::string("direct reader init failed: ") +
                          error.what(),
                .success = false,
                .log_csv_path = log_csv_path,
                .event_log_path = event_log_path,
            },
            naming);
        return 1;
    }

    std::ostringstream start_detail;
    start_detail << "evidence-log started"
                 << " poll_ms=" << poll_ms
                 << " staleness_threshold_ms=" << staleness_threshold_ms
                 << " evidence_gpu_sample_mode="
                 << config.evidence_gpu_sample_mode;
    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = "evidence-log",
            .event_type = "evidence_log.start",
            .detail = start_detail.str(),
            .success = true,
            .log_csv_path = log_csv_path,
            .event_log_path = event_log_path,
            .successful_polls = 0u,
            .skipped_polls = 0u,
            .stale = false,
        },
        naming);

    AmdReader amd_reader;
    GpuReader gpu_reader;
    std::uint64_t successful_polls = 0u;
    std::uint64_t skipped_polls = 0u;
    bool stale = false;
    auto last_success_time = std::chrono::steady_clock::now();

    while (!stop_requested.load() && !RuntimeStopRequested(runtime_home)) {
        try {
            RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
                amd_reader, gpu_reader, *fan_writer, runtime_policy);

            if (csv_logger.MaybeRotate()) {
                AppendRuntimeEvent(
                    runtime_home,
                    RuntimeLogEvent{
                        .mode = "evidence-log",
                        .event_type = "evidence_log.log_rotated",
                        .detail = "evidence log rotated",
                        .success = true,
                        .log_csv_path =
                            csv_logger.active_archive_path().string(),
                        .event_log_path = event_log_path,
                        .successful_polls = successful_polls,
                        .skipped_polls = skipped_polls,
                        .stale = stale,
                    },
                    naming);
            }

            const bool telemetry_available =
                RuntimeSnapshotHasTelemetry(runtime_snapshot);
            const FanTachEvidenceScanResult fan_tach_result =
                fan_writer->ReadFanTachEvidence();
            const SioVoltageScanResult voltage_result =
                fan_writer->ReadVoltages();
            const SioTemperatureScanResult temperature_result =
                fan_writer->ReadSioTemperatures();
            const GpuEvidenceSample gpu_evidence =
                gpu_reader.SampleEvidence(config.evidence_gpu_sample_mode);
            const bool sio_evidence_available =
                fan_tach_result || voltage_result || temperature_result;
            const std::string sio_evidence_detail =
                BuildSioEvidenceDetail(
                    fan_tach_result, voltage_result, temperature_result);
            std::string status_detail;
            if (telemetry_available) {
                last_success_time = std::chrono::steady_clock::now();
                ++successful_polls;
                stale = false;
                status_detail = "direct sample captured";
            } else {
                ++skipped_polls;
                status_detail = "direct sample had no telemetry";
            }

            const auto now = std::chrono::steady_clock::now();
            const auto since_success_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_success_time).count();
            stale = static_cast<std::uint64_t>(since_success_ms) >
                    static_cast<std::uint64_t>(staleness_threshold_ms);

            const bool row_written = csv_logger.WriteRow(BuildEvidenceLogCsvRow(
                runtime_snapshot,
                RuntimeEvidenceLogState{
                    .telemetry_available = telemetry_available,
                    .successful_polls = successful_polls,
                    .skipped_polls = skipped_polls,
                    .stale = stale,
                    .status_detail = status_detail,
                    .sio_evidence_available = sio_evidence_available,
                    .sio_evidence_detail = sio_evidence_detail,
                    .fan_tach_evidence =
                        ConvertFanTachEvidence(fan_tach_result),
                    .sio_voltages = ConvertVoltages(voltage_result),
                    .sio_temperatures =
                        ConvertTemperatures(temperature_result),
                    .gpu_evidence = gpu_evidence,
                }));
            if (!row_written) {
                AppendRuntimeEvent(
                    runtime_home,
                    RuntimeLogEvent{
                        .mode = "evidence-log",
                        .event_type = "evidence_log.write_failed",
                        .detail = "evidence CSV row write failed",
                        .success = false,
                        .snapshot_time_iso =
                            runtime_snapshot.snapshot_time_iso,
                        .log_csv_path =
                            csv_logger.active_archive_path().string(),
                        .event_log_path = event_log_path,
                        .successful_polls = successful_polls,
                        .skipped_polls = skipped_polls,
                        .stale = stale,
                        .telemetry_available = telemetry_available,
                    },
                    naming);
            }

            if (!telemetry_available) {
                AppendRuntimeEvent(
                    runtime_home,
                    RuntimeLogEvent{
                        .mode = "evidence-log",
                        .event_type = "evidence_log.sample_skipped",
                        .detail = status_detail,
                        .success = false,
                        .snapshot_time_iso =
                            runtime_snapshot.snapshot_time_iso,
                        .log_csv_path =
                            csv_logger.active_archive_path().string(),
                        .event_log_path = event_log_path,
                        .amd_sensor_count = static_cast<std::uint32_t>(
                            runtime_snapshot.amd_sensors.size()),
                        .fan_count = static_cast<std::uint32_t>(
                            runtime_snapshot.fans.size()),
                        .gpu_available = runtime_snapshot.gpu.available,
                        .successful_polls = successful_polls,
                        .skipped_polls = skipped_polls,
                        .stale = stale,
                        .telemetry_available = telemetry_available,
                    },
                    naming);
            }
        } catch (const std::exception& error) {
            ++skipped_polls;
            const std::string detail =
                std::string("direct sample failed: ") + error.what();
            AppendRuntimeEvent(
                runtime_home,
                RuntimeLogEvent{
                    .mode = "evidence-log",
                    .event_type = "evidence_log.sample_failed",
                    .detail = detail,
                    .success = false,
                    .log_csv_path = csv_logger.active_archive_path().string(),
                    .event_log_path = event_log_path,
                    .successful_polls = successful_polls,
                    .skipped_polls = skipped_polls,
                    .stale = stale,
                },
                naming);
        }

        WaitForNextEvidencePoll(runtime_home, stop_requested, poll_ms);
    }

    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = "evidence-log",
            .event_type = "evidence_log.shutdown",
            .detail = "stop requested",
            .success = true,
            .log_csv_path = csv_logger.active_archive_path().string(),
            .event_log_path = event_log_path,
            .successful_polls = successful_polls,
            .skipped_polls = skipped_polls,
            .stale = stale,
        },
        naming);
    csv_logger.Close();
    return 0;
}

}  // namespace svg_mb_control
