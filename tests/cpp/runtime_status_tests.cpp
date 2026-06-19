#include "runtime_status.h"

#include "runtime_event_log.h"
#include "test_helpers.h"

#include <nlohmann/json.hpp>

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

}  // namespace

int main() {
    TestStatusPublishFailureIsStickyAndRecovers();
    return g_failures == 0 ? 0 : 1;
}
