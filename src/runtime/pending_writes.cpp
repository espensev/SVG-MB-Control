#include "pending_writes.h"

#include "json_io.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

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

std::filesystem::path QuarantinedSidecarPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "pending_writes.json.corrupt";
}

TolerantPendingWritesRead ReadPendingWritesTolerant(
    const std::filesystem::path& runtime_home) {
    TolerantPendingWritesRead out;
    try {
        out.entries = ReadPendingWrites(runtime_home);
        return out;
    } catch (const std::exception& error) {
        // FEAT-0012 (REQ-SIDECARRESIL-01/02): a corrupt sidecar is quarantined
        // (renamed aside, preserving the bytes for forensics) and treated as
        // empty so startup is not fatal. This never throws on a corrupt file.
        const std::filesystem::path original =
            PendingWritesSidecarPath(runtime_home);
        const std::filesystem::path quarantine =
            QuarantinedSidecarPath(runtime_home);
        std::error_code ec;
        // std::filesystem::rename replaces an existing target on both POSIX and
        // Windows (MoveFileEx with REPLACE_EXISTING), keeping one most-recent
        // corrupt sample.
        std::filesystem::rename(original, quarantine, ec);
        out.entries.clear();
        out.quarantined = true;
        out.detail =
            std::string("corrupt pending-writes sidecar: ") + error.what();
        if (ec) {
            out.detail += std::string("; quarantine rename failed: ") +
                          ec.message();
        } else {
            out.detail += std::string("; quarantined to ") +
                          quarantine.filename().string();
        }
        return out;
    }
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

PendingWritesStore::PendingWritesStore(std::filesystem::path runtime_home)
    : runtime_home_(std::move(runtime_home)) {}

void PendingWritesStore::Load() {
    entries_ = ReadPendingWrites(runtime_home_);
    loaded_ = true;
    dirty_ = false;
}

void PendingWritesStore::Adopt(std::vector<PendingWriteEntry> entries) {
    entries_ = std::move(entries);
    loaded_ = true;
    dirty_ = false;
}

bool PendingWritesStore::Upsert(const PendingWriteEntry& entry) {
    if (!loaded_) {
        Load();
    }
    bool replaced = false;
    bool baseline_changed = false;
    for (auto& existing : entries_) {
        if (existing.channel == entry.channel) {
            baseline_changed =
                existing.baseline_duty_raw != entry.baseline_duty_raw ||
                existing.baseline_mode_raw != entry.baseline_mode_raw;
            existing = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        entries_.push_back(entry);
    }
    // FEAT-0019 (REQ-WRITEHOT-01/04): persist synchronously only when the
    // recovery-relevant identity changes — a new channel entry (first
    // activation) or a changed captured baseline. ReconcilePendingWrites
    // restores from (channel, baseline_duty_raw, baseline_mode_raw) only and
    // never reads target_pct, so a same-baseline target_pct/hold/started_iso
    // change does not need a synchronous write before ApplyDuty: it marks the
    // store dirty and the existing end-of-tick Flush() writes it (off the hot
    // path). The in-memory entry is updated above either way, so the FEAT-0010
    // non-vetoing actuation path is unaffected.
    if (!replaced || baseline_changed) {
        Persist();  // clears dirty_; throws on filesystem failure
        return true;
    }
    dirty_ = true;
    return false;
}

void PendingWritesStore::QueueRemove(std::uint32_t channel) {
    if (!loaded_) {
        Load();
    }
    const auto before = entries_.size();
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [channel](const PendingWriteEntry& entry) {
                           return entry.channel == channel;
                       }),
        entries_.end());
    if (entries_.size() != before) {
        dirty_ = true;
    }
}

bool PendingWritesStore::Flush() {
    if (!dirty_) {
        return false;
    }
    Persist();  // clears dirty_; throws on filesystem failure
    return true;
}

void PendingWritesStore::Persist() {
    WritePendingWrites(runtime_home_, entries_);
    dirty_ = false;
}

}  // namespace svg_mb_control
