#include "runtime_event_log.h"

#include "test_helpers.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path MakeTempRuntimeHome(const std::string& name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("svg_mb_control_runtime_event_log_tests_" + name + "_" +
         UniqueTempSuffix());
    std::filesystem::create_directories(root / "logs" / "archive");
    return root;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream stream(path, std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool Contains(const std::vector<std::string>& lines,
              const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void TestRotationUsesActiveStartAndPrunesArchives() {
    using namespace std::chrono_literals;
    const auto runtime_home = MakeTempRuntimeHome("rotate");
    const auto event_path =
        svg_mb_control::ResolveRuntimeEventLogPath(runtime_home);
    const auto archive_dir = runtime_home / "logs" / "archive";
    const auto stale_archive =
        archive_dir / "svg_mb_control_events_20000101_000000.jsonl";

    svg_mb_control::RuntimeLogEvent old_event;
    old_event.mode = "control-loop";
    old_event.event_type = "control_loop.start";
    old_event.detail = "old event";
    old_event.success = true;
    ExpectTrue(svg_mb_control::AppendRuntimeEvent(runtime_home, old_event),
               "initial append succeeds");

    std::ofstream stale(stale_archive, std::ios::binary);
    stale << "{}\n";
    stale.close();

    std::filesystem::last_write_time(
        event_path,
        std::filesystem::file_time_type::clock::now());
    std::filesystem::last_write_time(
        stale_archive,
        std::filesystem::file_time_type::clock::now() - 48h);

    svg_mb_control::ConfigureRuntimeEventLogRetention(
        event_path,
        svg_mb_control::RuntimeEventLogOptions{
            .rotate_hours = 1u,
            .retain_days = 1u,
            .active_started_at = std::chrono::system_clock::now() - 2h,
        });

    svg_mb_control::RuntimeLogEvent new_event;
    new_event.mode = "control-loop";
    new_event.event_type = "control_loop.shutdown";
    new_event.detail = "new event";
    new_event.success = true;
    ExpectTrue(svg_mb_control::AppendRuntimeEvent(runtime_home, new_event),
               "append after rotation succeeds");

    const auto active_lines = ReadLines(event_path);
    ExpectTrue(active_lines.size() == 1u,
               "active event log has one post-rotation line");
    ExpectTrue(Contains(active_lines, "control_loop.shutdown"),
               "active event log contains new event");
    ExpectFalse(std::filesystem::exists(stale_archive),
                "old event archive was pruned");

    int archive_jsonl_count = 0;
    bool found_rotated_old_event = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(archive_dir)) {
        if (entry.path().extension() != ".jsonl") {
            continue;
        }
        ++archive_jsonl_count;
        found_rotated_old_event =
            found_rotated_old_event ||
            Contains(ReadLines(entry.path()), "control_loop.start");
    }
    ExpectTrue(archive_jsonl_count == 1,
               "exactly one retained event archive remains");
    ExpectTrue(found_rotated_old_event,
               "rotated archive contains the pre-rotation event");

    std::filesystem::remove_all(runtime_home);
}

void TestWriteAppliedReductionPreservesDiagnostics() {
    const auto runtime_home = MakeTempRuntimeHome("sample");
    const auto event_path =
        svg_mb_control::ResolveRuntimeEventLogPath(runtime_home);

    svg_mb_control::ConfigureRuntimeEventLogRetention(
        event_path,
        svg_mb_control::RuntimeEventLogOptions{
            .reduce_routine_write_applied = true,
            .write_applied_sample_interval = 3u,
        });

    for (std::uint64_t tick = 1u; tick <= 5u; ++tick) {
        svg_mb_control::RuntimeLogEvent event;
        event.event_type = "control_loop.write_applied";
        event.detail = "routine write";
        event.tick_count = tick;
        event.success = true;
        ExpectTrue(svg_mb_control::AppendControlLoopEvent(runtime_home, event),
                   "write_applied append returns success");
    }

    svg_mb_control::RuntimeLogEvent warning;
    warning.event_type = "control_loop.write_failed";
    warning.detail = "diagnostic event";
    warning.tick_count = 6u;
    warning.success = false;
    ExpectTrue(svg_mb_control::AppendControlLoopEvent(runtime_home, warning),
               "diagnostic append succeeds");

    svg_mb_control::RuntimeLogEvent lifecycle;
    lifecycle.event_type = "control_loop.shutdown";
    lifecycle.detail = "lifecycle event";
    lifecycle.success = true;
    ExpectTrue(svg_mb_control::AppendControlLoopEvent(runtime_home, lifecycle),
               "lifecycle append succeeds");

    const auto lines = ReadLines(event_path);
    ExpectTrue(lines.size() == 4u,
               "write_applied reduction keeps first/sample plus diagnostics");
    ExpectTrue(Contains(lines, "\"tick_count\":1"),
               "first write_applied event is retained");
    ExpectTrue(Contains(lines, "\"tick_count\":3"),
               "sampled write_applied event is retained");
    ExpectFalse(Contains(lines, "\"tick_count\":2"),
                "routine write_applied tick 2 is reduced out");
    ExpectFalse(Contains(lines, "\"tick_count\":4"),
                "routine write_applied tick 4 is reduced out");
    ExpectTrue(Contains(lines, "control_loop.write_failed"),
               "diagnostic event is retained");
    ExpectTrue(Contains(lines, "control_loop.shutdown"),
               "lifecycle event is retained");

    std::filesystem::remove_all(runtime_home);
}

void TestConcurrentAppendRotationKeepsWholeLines() {
    using namespace std::chrono_literals;
    const auto runtime_home = MakeTempRuntimeHome("concurrent");
    const auto event_path =
        svg_mb_control::ResolveRuntimeEventLogPath(runtime_home);
    const auto archive_dir = runtime_home / "logs" / "archive";

    svg_mb_control::RuntimeLogEvent seed;
    seed.mode = "control-loop";
    seed.event_type = "control_loop.start";
    seed.detail = "seed event";
    seed.success = true;
    ExpectTrue(svg_mb_control::AppendRuntimeEvent(runtime_home, seed),
               "seed append succeeds");
    std::filesystem::last_write_time(
        event_path,
        std::filesystem::file_time_type::clock::now());

    svg_mb_control::ConfigureRuntimeEventLogRetention(
        event_path,
        svg_mb_control::RuntimeEventLogOptions{
            .rotate_hours = 1u,
            .retain_days = 1u,
            .active_started_at = std::chrono::system_clock::now() - 2h,
        });

    std::vector<int> append_results(8, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([runtime_home, &append_results, i]() {
            svg_mb_control::RuntimeLogEvent event;
            event.mode = "control-loop";
            event.event_type = "control_loop.shutdown";
            event.detail = "thread event " + std::to_string(i);
            event.success = true;
            append_results[static_cast<std::size_t>(i)] =
                svg_mb_control::AppendRuntimeEvent(runtime_home, event) ? 1 : 0;
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const int result : append_results) {
        ExpectTrue(result == 1, "thread append succeeds");
    }

    std::vector<std::string> all_lines = ReadLines(event_path);
    for (const auto& entry :
         std::filesystem::directory_iterator(archive_dir)) {
        if (entry.path().extension() == ".jsonl") {
            const auto archive_lines = ReadLines(entry.path());
            all_lines.insert(
                all_lines.end(), archive_lines.begin(), archive_lines.end());
        }
    }

    int seed_count = 0;
    int thread_count = 0;
    for (const auto& line : all_lines) {
        const auto parsed = nlohmann::json::parse(line);
        const std::string detail = parsed.value("detail", "");
        if (detail == "seed event") {
            ++seed_count;
        }
        if (detail.find("thread event ") == 0) {
            ++thread_count;
        }
    }
    ExpectTrue(seed_count == 1, "rotated seed line remains whole JSON");
    ExpectTrue(thread_count == 8, "all concurrent append lines remain whole JSON");

    std::filesystem::remove_all(runtime_home);
}

}  // namespace

int main() {
    TestRotationUsesActiveStartAndPrunesArchives();
    TestWriteAppliedReductionPreservesDiagnostics();
    TestConcurrentAppendRotationKeepsWholeLines();
    return g_failures == 0 ? 0 : 1;
}
