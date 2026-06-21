#include "runtime_status.h"

#include "runtime_event_log.h"
#include "runtime_health.h"
#include "runtime_util.h"
#include "test_helpers.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path MakeTempRuntimeHome(const std::string& name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("svg_mb_control_runtime_status_tests_" + name + "_" +
         UniqueTempSuffix());
    std::filesystem::create_directories(root / "logs");
    return root;
}

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& home) {
    std::vector<nlohmann::json> events;
    std::ifstream stream(svg_mb_control::ResolveRuntimeEventLogPath(home),
                         std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            events.push_back(nlohmann::json::parse(line));
        }
    }
    return events;
}

int CountEvents(const std::vector<nlohmann::json>& events,
                const std::string& event_type) {
    int count = 0;
    for (const auto& event : events) {
        if (event.value("event_type", "") == event_type) {
            ++count;
        }
    }
    return count;
}

bool WriteMinimalControlStatus(const std::filesystem::path& runtime_home) {
    svg_mb_control::RuntimeControlLoopTimingState timing;
    std::vector<svg_mb_control::ChannelState> channels;
    return svg_mb_control::WriteControlLoopStatus(
        runtime_home,
        "control-loop",
        "running",
        "test status",
        1u,
        "2026-06-20T10:00:00",
        timing,
        channels,
        "",
        "",
        svg_mb_control::ResolveRuntimeEventLogPath(runtime_home).string());
}

bool WriteControlStatusWithHardwareAccess(
    const std::filesystem::path& runtime_home,
    const svg_mb_control::HardwareAccessStatus& hardware_access) {
    svg_mb_control::RuntimeControlLoopTimingState timing;
    std::vector<svg_mb_control::ChannelState> channels;
    return svg_mb_control::WriteControlLoopStatus(
        runtime_home,
        "control-loop",
        "running",
        "test status",
        1u,
        svg_mb_control::FormatLocalIso8601(
            std::chrono::system_clock::now()),
        timing,
        channels,
        "",
        "",
        svg_mb_control::ResolveRuntimeEventLogPath(runtime_home).string(),
        "",
        "",
        "",
        hardware_access);
}

void TestStatusPublishFailureIsStickyAndRecovers() {
    const auto runtime_home = MakeTempRuntimeHome("control_status");
    const auto status_path = svg_mb_control::RuntimeStatusPath(runtime_home);
    std::filesystem::create_directories(status_path);

    ExpectFalse(WriteMinimalControlStatus(runtime_home),
                "status write fails when control_runtime.json is a directory");
    ExpectFalse(WriteMinimalControlStatus(runtime_home),
                "second status write still fails while path is blocked");

    auto events = ReadEvents(runtime_home);
    ExpectTrue(CountEvents(events,
                           "runtime_logging.status_publish_failed") == 1,
               "persistent status failure emits one sticky failure event");
    ExpectTrue(CountEvents(events,
                           "runtime_logging.status_publish_recovered") == 0,
               "no recovery event while status path is still blocked");

    std::filesystem::remove_all(status_path);
    ExpectTrue(WriteMinimalControlStatus(runtime_home),
               "status write succeeds after path is unblocked");
    events = ReadEvents(runtime_home);
    ExpectTrue(CountEvents(events,
                           "runtime_logging.status_publish_failed") == 1,
               "recovery does not duplicate the failure event");
    ExpectTrue(CountEvents(events,
                           "runtime_logging.status_publish_recovered") == 1,
               "status recovery emits one recovery event");
    ExpectTrue(std::filesystem::is_regular_file(status_path),
               "status file exists after recovery");

    std::filesystem::remove_all(runtime_home);
}

void TestHardwareAccessStatusRoundTrip() {
    const auto runtime_home = MakeTempRuntimeHome("hwaccess_roundtrip");
    svg_mb_control::HardwareAccessStatus hardware_access;
    hardware_access.read_state =
        svg_mb_control::HardwareAccessState::kAvailable;
    hardware_access.read_detail = "AMD/SMN reader initialized";
    hardware_access.write_state =
        svg_mb_control::HardwareAccessState::kUnavailable;
    hardware_access.write_detail = "svg_mb_sio init failed: PawnIO absent";

    ExpectTrue(WriteControlStatusWithHardwareAccess(runtime_home,
                                                    hardware_access),
               "control status with hardware access writes");

    const nlohmann::json payload = [&] {
        std::ifstream stream(svg_mb_control::RuntimeStatusPath(runtime_home),
                             std::ios::binary);
        return nlohmann::json::parse(stream);
    }();
    ExpectEqual(payload.value("hwaccess_state", ""), "unavailable",
                "status JSON exposes overall unavailable hardware access");
    ExpectEqual(payload.value("hwaccess_read_state", ""), "available",
                "status JSON exposes read-path availability");
    ExpectEqual(payload.value("hwaccess_write_state", ""), "unavailable",
                "status JSON exposes write-path unavailability");
    ExpectEqual(payload.value("hwaccess_write_detail", ""),
                "svg_mb_sio init failed: PawnIO absent",
                "status JSON preserves write-path detail");

    const auto snapshot = svg_mb_control::ReadRuntimeStatus(runtime_home);
    ExpectTrue(snapshot.has_value(), "typed status reads status JSON");
    ExpectEqual(
        svg_mb_control::HardwareAccessStateName(
            svg_mb_control::HardwareAccessOverallState(
                snapshot->hardware_access)),
        "unavailable",
        "typed status preserves overall hardware access state");
    ExpectEqual(
        svg_mb_control::HardwareAccessStateName(
            snapshot->hardware_access.read_state),
        "available",
        "typed status preserves read-path hardware access state");
    ExpectEqual(
        svg_mb_control::HardwareAccessStateName(
            snapshot->hardware_access.write_state),
        "unavailable",
        "typed status preserves write-path hardware access state");

    const svg_mb_control::RuntimeHealthResult health =
        svg_mb_control::EvaluateRuntimeHealth(runtime_home, 60000u);
    const nlohmann::json health_json =
        svg_mb_control::RuntimeHealthToJson(health);
    ExpectEqual(health_json.value("hwaccess_state", ""), "unavailable",
                "health JSON exposes hardware access state");
    ExpectEqual(health_json.value("hwaccess_write_detail", ""),
                "svg_mb_sio init failed: PawnIO absent",
                "health JSON preserves write-path detail");
    ExpectTrue(svg_mb_control::RuntimeHealthExitCode(health.state) == 0,
               "hardware access reporting does not change health exit code");
    std::filesystem::remove_all(runtime_home);
}

void TestDefaultHardwareAccessIsUnknown() {
    const auto runtime_home = MakeTempRuntimeHome("hwaccess_unknown");

    ExpectTrue(WriteMinimalControlStatus(runtime_home),
               "control status with default hardware access writes");

    const nlohmann::json payload = [&] {
        std::ifstream stream(svg_mb_control::RuntimeStatusPath(runtime_home),
                             std::ios::binary);
        return nlohmann::json::parse(stream);
    }();
    ExpectEqual(payload.value("hwaccess_state", ""), "unknown",
                "default hardware access overall is unknown");
    ExpectEqual(payload.value("hwaccess_read_state", ""), "unknown",
                "default read-path hardware access is unknown");
    ExpectEqual(payload.value("hwaccess_write_state", ""), "unknown",
                "default write-path hardware access is unknown");
    ExpectEqual(
        svg_mb_control::HardwareAccessStateName(
            svg_mb_control::HardwareAccessOverallState(
                svg_mb_control::HardwareAccessStatus{})),
        "unknown",
        "absence of a successful open is never reported as available");

    std::filesystem::remove_all(runtime_home);
}

void TestHardwareAccessTransitionClassification() {
    svg_mb_control::HardwareAccessStatus unknown;
    svg_mb_control::HardwareAccessStatus unavailable;
    unavailable.read_state =
        svg_mb_control::HardwareAccessState::kUnavailable;
    unavailable.write_state =
        svg_mb_control::HardwareAccessState::kUnknown;
    svg_mb_control::HardwareAccessStatus available;
    available.read_state = svg_mb_control::HardwareAccessState::kAvailable;
    available.write_state = svg_mb_control::HardwareAccessState::kAvailable;

    ExpectTrue(svg_mb_control::ClassifyHardwareAccessTransition(
                   unknown, unavailable) ==
                   svg_mb_control::HardwareAccessTransition::kUnavailable,
               "unknown to unavailable emits unavailable transition");
    ExpectTrue(svg_mb_control::ClassifyHardwareAccessTransition(
                   unavailable, available) ==
                   svg_mb_control::HardwareAccessTransition::kRestored,
               "unavailable to available emits restored transition");
    ExpectTrue(svg_mb_control::ClassifyHardwareAccessTransition(
                   unknown, available) ==
                   svg_mb_control::HardwareAccessTransition::kNone,
               "unknown to available does not emit restored transition");
}

}  // namespace

int main() {
    TestStatusPublishFailureIsStickyAndRecovers();
    TestHardwareAccessStatusRoundTrip();
    TestDefaultHardwareAccessIsUnknown();
    TestHardwareAccessTransitionClassification();
    return g_failures == 0 ? 0 : 1;
}
