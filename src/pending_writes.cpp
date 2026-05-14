#include "pending_writes.h"

#include "json_io.h"

#include <stdexcept>
#include <system_error>

namespace svg_mb_control {

namespace {

PendingWriteEntry PendingWriteEntryFromJson(const nlohmann::json& item) {
    PendingWriteEntry entry;
    entry.channel = item.at("channel").get<std::uint32_t>();
    entry.baseline_duty_raw = static_cast<std::uint8_t>(
        item.at("baseline_duty_raw").get<unsigned int>());
    entry.baseline_mode_raw = static_cast<std::uint8_t>(
        item.at("baseline_mode_raw").get<unsigned int>());
    entry.target_pct = item.at("target_pct").get<double>();
    entry.requested_hold_ms = item.at("requested_hold_ms").get<std::uint32_t>();
    entry.started_iso = item.value("started_iso", std::string());
    entry.child_pid = item.value("child_pid", 0u);
    return entry;
}

nlohmann::json PendingWriteEntryToJson(const PendingWriteEntry& entry) {
    return {
        {"channel", entry.channel},
        {"baseline_duty_raw", static_cast<unsigned int>(entry.baseline_duty_raw)},
        {"baseline_mode_raw", static_cast<unsigned int>(entry.baseline_mode_raw)},
        {"target_pct", entry.target_pct},
        {"requested_hold_ms", entry.requested_hold_ms},
        {"started_iso", entry.started_iso},
        {"child_pid", entry.child_pid},
    };
}

}  // namespace

std::filesystem::path PendingWritesSidecarPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "pending_writes.json";
}

std::vector<PendingWriteEntry> ReadPendingWrites(
    const std::filesystem::path& runtime_home) {
    const std::filesystem::path path = PendingWritesSidecarPath(runtime_home);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }

    const nlohmann::json root = ReadJsonFile(path, "pending-writes sidecar");
    const auto entries_it = root.find("entries");
    if (entries_it == root.end()) {
        return {};
    }
    if (!entries_it->is_array()) {
        throw std::runtime_error("pending-writes sidecar missing entries array.");
    }

    std::vector<PendingWriteEntry> result;
    result.reserve(entries_it->size());
    for (const auto& item : *entries_it) {
        result.push_back(PendingWriteEntryFromJson(item));
    }
    return result;
}

void WritePendingWrites(const std::filesystem::path& runtime_home,
                        const std::vector<PendingWriteEntry>& entries) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["entries"] = nlohmann::json::array();
    for (const auto& entry : entries) {
        payload["entries"].push_back(PendingWriteEntryToJson(entry));
    }
    WriteJsonFileAtomic(PendingWritesSidecarPath(runtime_home), payload);
}

void UpsertPendingWrite(const std::filesystem::path& runtime_home,
                        const PendingWriteEntry& entry) {
    std::vector<PendingWriteEntry> entries = ReadPendingWrites(runtime_home);
    for (auto& existing : entries) {
        if (existing.channel == entry.channel) {
            existing = entry;
            WritePendingWrites(runtime_home, entries);
            return;
        }
    }
    entries.push_back(entry);
    WritePendingWrites(runtime_home, entries);
}

void RemovePendingWrite(const std::filesystem::path& runtime_home,
                        std::uint32_t channel) {
    std::vector<PendingWriteEntry> entries = ReadPendingWrites(runtime_home);
    std::vector<PendingWriteEntry> remaining;
    remaining.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.channel != channel) {
            remaining.push_back(entry);
        }
    }
    if (remaining.size() == entries.size()) {
        return;
    }
    WritePendingWrites(runtime_home, remaining);
}

}  // namespace svg_mb_control
