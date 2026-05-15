#include "analyze_json_artifacts.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace svg_mb_control::analyze {

namespace {

constexpr const char* kManifestSchema =
    "svg_mb_control.runtime_log_manifest.v1";
constexpr const char* kEventSchema = "svg_mb_control.event.v1";
constexpr const char* kPlantModelSchema =
    "svg_mb_control.plant_model_capture.v1";

std::optional<std::string> JsonOptionalString(const nlohmann::json& node,
                                              const char* key) {
    auto it = node.find(key);
    if (it == node.end() || it->is_null()) {
        return std::nullopt;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::int64_t> JsonOptionalInt(const nlohmann::json& node,
                                            const char* key) {
    auto it = node.find(key);
    if (it == node.end() || it->is_null()) {
        return std::nullopt;
    }
    if (it->is_number_integer() || it->is_number_unsigned()) {
        return it->get<std::int64_t>();
    }
    if (it->is_boolean()) {
        return it->get<bool>() ? std::int64_t{1} : std::int64_t{0};
    }
    if (it->is_number_float()) {
        return static_cast<std::int64_t>(it->get<double>());
    }
    return std::nullopt;
}

std::optional<double> JsonOptionalDouble(const nlohmann::json& node,
                                         const char* key) {
    auto it = node.find(key);
    if (it == node.end() || it->is_null()) {
        return std::nullopt;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    return std::nullopt;
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open JSON file: " + path.string());
    }
    try {
        return nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error& error) {
        throw std::runtime_error("Failed to parse JSON " + path.string() +
                                 ": " + error.what());
    }
}

}  // namespace

ManifestData ParseRuntimeManifest(const std::filesystem::path& path) {
    const nlohmann::json doc = ReadJsonFile(path);

    const std::string schema = doc.value("schema", std::string{});
    if (schema != kManifestSchema) {
        throw std::runtime_error("Unexpected manifest schema in " +
                                 path.string() + ": '" + schema +
                                 "' (expected '" + kManifestSchema + "')");
    }

    ManifestData out;
    out.session_start = doc.value("session_start", std::string{});
    out.mode = doc.value("mode", std::string{});
    out.status = doc.value("status", std::string{});
    out.row_count = JsonOptionalInt(doc, "row_count");
    out.event_count = JsonOptionalInt(doc, "event_count");
    out.last_update = JsonOptionalString(doc, "last_update");

    if (auto producer = doc.find("producer");
        producer != doc.end() && producer->is_object()) {
        out.tool_version = JsonOptionalString(*producer, "version");
        out.git_hash = JsonOptionalString(*producer, "git_hash");
    }
    if (auto writer = doc.find("writer");
        writer != doc.end() && writer->is_object()) {
        out.csv_flush_policy = JsonOptionalString(*writer, "csv_flush_policy");
        out.mirror_mode = JsonOptionalString(*writer, "mirror_mode");
    }
    if (auto artifacts = doc.find("artifacts");
        artifacts != doc.end() && artifacts->is_object()) {
        if (auto archive = artifacts->find("csv_archive");
            archive != artifacts->end() && archive->is_object()) {
            if (auto p = JsonOptionalString(*archive, "path"); p.has_value()) {
                out.csv_archive_path = std::filesystem::path(*p);
            }
        }
        if (!out.csv_archive_path.has_value()) {
            if (auto latest = artifacts->find("csv_latest");
                latest != artifacts->end() && latest->is_object()) {
                if (auto p = JsonOptionalString(*latest, "path");
                    p.has_value()) {
                    out.csv_archive_path = std::filesystem::path(*p);
                }
            }
        }
    }

    if (out.session_start.empty() || out.mode.empty() || out.status.empty()) {
        throw std::runtime_error(
            "Manifest missing required field (session_start/mode/status): " +
            path.string());
    }

    return out;
}

std::vector<EventData> ParseEventsJsonl(const std::filesystem::path& path) {
    std::vector<EventData> out;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return out;
    }

    std::string line;
    std::size_t line_no = 0u;
    while (std::getline(stream, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        nlohmann::json doc;
        try {
            doc = nlohmann::json::parse(line);
        } catch (const nlohmann::json::parse_error&) {
            continue;
        }
        if (!doc.is_object()) {
            continue;
        }
        const std::string schema = doc.value("schema", std::string{});
        if (!schema.empty() && schema != kEventSchema) {
            continue;
        }

        EventData event;
        event.event_time = doc.value("event_time", std::string{});
        event.event_type = doc.value("event_type", std::string{});
        if (event.event_time.empty() || event.event_type.empty()) {
            continue;
        }
        event.mode = JsonOptionalString(doc, "mode");
        event.success = JsonOptionalInt(doc, "success");
        event.channel = JsonOptionalInt(doc, "channel");
        event.setpoint_pct = JsonOptionalDouble(doc, "setpoint_pct");
        event.observed_temp_c = JsonOptionalDouble(doc, "observed_temp_c");
        event.tick_count = JsonOptionalInt(doc, "tick_count");
        event.detail = JsonOptionalString(doc, "detail");

        nlohmann::json extras = doc;
        for (const char* key : {"event_time", "event_type", "mode", "success",
                                "channel", "setpoint_pct", "observed_temp_c",
                                "tick_count", "detail", "schema"}) {
            extras.erase(key);
        }
        if (!extras.empty()) {
            event.extra_json = extras.dump();
        }
        out.push_back(std::move(event));
    }
    return out;
}

PlantModelData ParsePlantModelCapture(const std::filesystem::path& path) {
    const nlohmann::json doc = ReadJsonFile(path);

    const std::string schema = doc.value("schema", std::string{});
    if (schema != kPlantModelSchema) {
        throw std::runtime_error("Unexpected plant model schema in " +
                                 path.string() + ": '" + schema +
                                 "' (expected '" + kPlantModelSchema + "')");
    }

    PlantModelData out;
    out.captured_local = doc.value("captured_local", std::string{});
    out.abort_reason = JsonOptionalString(doc, "abort_reason");

    if (auto producer = doc.find("producer");
        producer != doc.end() && producer->is_object()) {
        out.tool_version = JsonOptionalString(*producer, "version");
        out.git_hash = JsonOptionalString(*producer, "git_hash");
    }

    if (auto options = doc.find("options");
        options != doc.end() && options->is_object()) {
        if (auto sw = JsonOptionalInt(*options, "settle_window_ms");
            sw.has_value()) {
            out.settle_window_ms = *sw;
        }
        if (auto ceil = JsonOptionalDouble(*options, "abort_temp_ceiling_c");
            ceil.has_value()) {
            out.abort_temp_ceiling_c = *ceil;
        }
        if (auto seq = options->find("sequence");
            seq != options->end() && seq->is_array()) {
            out.sequence_json = seq->dump();
        }
    }

    if (auto channels = doc.find("channels");
        channels != doc.end() && channels->is_array()) {
        for (const auto& channel_node : *channels) {
            if (!channel_node.is_object()) {
                continue;
            }
            PlantModelChannel ch;
            ch.channel = channel_node.value("channel", std::int64_t{0});
            ch.baseline_captured = JsonOptionalInt(channel_node,
                                                   "baseline_captured");
            ch.restored = JsonOptionalInt(channel_node, "restored");
            ch.note = JsonOptionalString(channel_node, "note");

            if (auto baseline = channel_node.find("baseline");
                baseline != channel_node.end() && baseline->is_object()) {
                ch.baseline_duty_raw = JsonOptionalInt(*baseline, "duty_raw");
                ch.baseline_mode_raw = JsonOptionalInt(*baseline, "mode_raw");
                ch.baseline_rpm = JsonOptionalDouble(*baseline, "rpm");
                ch.baseline_tctl_c = JsonOptionalDouble(*baseline, "tctl_c");
                ch.baseline_gpu_envelope_c = JsonOptionalDouble(
                    *baseline, "gpu_envelope_c");
            }

            if (auto steps = channel_node.find("steps");
                steps != channel_node.end() && steps->is_array()) {
                std::int64_t step_index = 0;
                for (const auto& step_node : *steps) {
                    if (!step_node.is_object()) {
                        ++step_index;
                        continue;
                    }
                    PlantModelStep step;
                    step.step_index = step_index++;
                    step.duty_pct_target = step_node.value(
                        "duty_pct_target", 0.0);
                    step.hold_ms = step_node.value("hold_ms", std::int64_t{0});
                    step.settle_window_ms = step_node.value(
                        "settle_window_ms", std::int64_t{0});
                    step.settle_sample_count = JsonOptionalInt(
                        step_node, "settle_sample_count");
                    step.duty_pct_observed_mean = JsonOptionalDouble(
                        step_node, "duty_pct_observed_mean");
                    step.rpm_mean = JsonOptionalDouble(step_node, "rpm_mean");
                    step.rpm_stddev = JsonOptionalDouble(step_node,
                                                          "rpm_stddev");
                    step.tctl_c_mean = JsonOptionalDouble(step_node,
                                                           "tctl_c_mean");
                    step.gpu_envelope_c_mean = JsonOptionalDouble(
                        step_node, "gpu_envelope_c_mean");
                    ch.steps.push_back(std::move(step));
                }
            }

            out.channels.push_back(std::move(ch));
        }
    }

    if (out.captured_local.empty()) {
        throw std::runtime_error(
            "Plant model missing required field (captured_local): " +
            path.string());
    }
    if (out.sequence_json.empty()) {
        out.sequence_json = "[]";
    }

    return out;
}

}  // namespace svg_mb_control::analyze
