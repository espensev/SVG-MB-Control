#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::filesystem::path executable_directory(const char* argv0) {
    return std::filesystem::absolute(std::filesystem::path(argv0)).parent_path();
}

std::string env_value(const char* name, const std::string& fallback) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
        std::string text(value);
        free(value);
        return text;
    }
    return fallback;
}

unsigned long env_ulong(const char* name, unsigned long fallback) {
    const std::string text = env_value(name, "");
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoul(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string fake_mode() {
    return env_value("SVG_MB_CONTROL_FAKE_MODE", "success");
}

void write_complete_snapshot(const std::filesystem::path& path,
                             const std::string& command,
                             const std::string& mode,
                             unsigned long duration_ms,
                             unsigned long sample_index) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open fake snapshot file.");
    }

    stream
        << "{\n"
        << "  \"source\": \"fake-bench\",\n"
        << "  \"command\": \"" << command << "\",\n"
        << "  \"mode\": \"" << mode << "\",\n"
        << "  \"duration_ms\": " << duration_ms << ",\n"
        << "  \"sample\": " << sample_index << "\n"
        << "}\n";
}

void write_torn_snapshot(const std::filesystem::path& path,
                         unsigned long torn_hold_ms) {
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            throw std::runtime_error("Could not open fake snapshot file for torn write.");
        }
        stream << "{";
        stream.flush();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(torn_hold_ms));
}

void emit_summary(const std::string& command,
                  const std::string& mode,
                  unsigned long duration_ms,
                  const std::filesystem::path& snapshot_path) {
    std::cout << command << '\n';
    std::cout << "runtime_policy_path: (defaults)\n";
    std::cout << "amd_capture: 1\n";
    std::cout << "sio_capture: 1\n";
    std::cout << "duration_ms: " << duration_ms << '\n';
    if (mode != "missing_snapshot_path" && command == "read-snapshot") {
        std::cout << "snapshot_archive: " << snapshot_path.string() << '\n';
    }
    if (mode != "missing_snapshot_path" && command == "logger-service") {
        std::cout << "snapshot_path: " << snapshot_path.string() << '\n';
    }
    std::cout << "manifest_archive: fake_manifest.json\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "fake-bench requires a bridge command\n";
            return 2;
        }

        const std::string command = argv[1];
        if (command != "read-snapshot" && command != "logger-service") {
            std::cerr << "fake-bench supports read-snapshot or logger-service\n";
            return 2;
        }

        const std::string mode = fake_mode();
        if (mode == "fail_exit") {
            std::cerr << "simulated " << command << " failure\n";
            return 7;
        }

        unsigned long duration_ms = 0ul;
        for (int index = 2; index < argc; ++index) {
            const std::string arg = argv[index];
            if (arg == "--duration-ms") {
                if (index + 1 >= argc) {
                    std::cerr << "fake-bench missing value for --duration-ms\n";
                    return 3;
                }
                duration_ms = std::stoul(argv[++index]);
            }
        }

        const std::filesystem::path snapshot_path = executable_directory(argv[0]) /
            (command == "logger-service"
                 ? "fake_logger_service_current_state.json"
                 : "fake_read_snapshot_snapshot.json");

        if (mode == "emit_snapshots" || mode == "torn_writes") {
            const unsigned long publish_interval_ms = env_ulong(
                "SVG_MB_CONTROL_FAKE_PUBLISH_INTERVAL_MS", 100ul);
            const unsigned long torn_hold_ms = env_ulong(
                "SVG_MB_CONTROL_FAKE_TORN_HOLD_MS", 20ul);
            const auto start = std::chrono::steady_clock::now();
            unsigned long sample_index = 0ul;
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start).count();
                if (static_cast<unsigned long>(elapsed_ms) >= duration_ms) {
                    break;
                }
                ++sample_index;
                if (mode == "torn_writes") {
                    write_torn_snapshot(snapshot_path, torn_hold_ms);
                }
                write_complete_snapshot(snapshot_path, command, mode,
                                        duration_ms, sample_index);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(publish_interval_ms));
            }
            emit_summary(command, mode, duration_ms, snapshot_path);
            return 0;
        }

        if (mode == "crash_after_ms") {
            const unsigned long crash_after_ms = env_ulong(
                "SVG_MB_CONTROL_FAKE_CRASH_AFTER_MS", 200ul);
            write_complete_snapshot(snapshot_path, command, mode, duration_ms, 1ul);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(crash_after_ms));
            return 11;
        }

        if (mode == "idle_after_emit") {
            write_complete_snapshot(snapshot_path, command, mode, duration_ms, 1ul);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(duration_ms));
            emit_summary(command, mode, duration_ms, snapshot_path);
            return 0;
        }

        if (mode == "success") {
            write_complete_snapshot(snapshot_path, command, mode, duration_ms, 1ul);
        } else if (mode == "missing_snapshot_file") {
            std::error_code ec;
            std::filesystem::remove(snapshot_path, ec);
        } else if (mode == "missing_snapshot_path") {
            write_complete_snapshot(snapshot_path, command, mode, duration_ms, 1ul);
        } else {
            write_complete_snapshot(snapshot_path, command, mode, duration_ms, 1ul);
        }

        emit_summary(command, mode, duration_ms, snapshot_path);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fake-bench error: " << error.what() << '\n';
        return 9;
    }
}
