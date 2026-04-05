#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
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

std::string format_local_iso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    if (written == 0u) {
        return {};
    }
    return std::string(buffer.data(), written);
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

    const long snapshot_offset_ms = static_cast<long>(env_ulong(
        "SVG_MB_CONTROL_FAKE_SNAPSHOT_OFFSET_MS", 0ul));
    const auto snapshot_time = std::chrono::system_clock::now() -
        std::chrono::milliseconds(snapshot_offset_ms);
    const std::string snapshot_time_iso = format_local_iso8601(snapshot_time);

    const std::string policy_writes_enabled = env_value(
        "SVG_MB_CONTROL_FAKE_POLICY_WRITES_ENABLED", "true");
    const std::string policy_restore_on_exit = env_value(
        "SVG_MB_CONTROL_FAKE_POLICY_RESTORE_ON_EXIT", "true");
    const unsigned long fan_channel = env_ulong(
        "SVG_MB_CONTROL_FAKE_FAN_CHANNEL", 0ul);
    const unsigned long fan_duty_raw = env_ulong(
        "SVG_MB_CONTROL_FAKE_FAN_DUTY_RAW", 128ul);
    const unsigned long fan_mode_raw = env_ulong(
        "SVG_MB_CONTROL_FAKE_FAN_MODE_RAW", 5ul);
    const std::string effective_write =
        env_value("SVG_MB_CONTROL_FAKE_EFFECTIVE_WRITE_ALLOWED", "true");
    const std::string write_allowed =
        env_value("SVG_MB_CONTROL_FAKE_WRITE_ALLOWED", "true");
    const std::string policy_blocked =
        env_value("SVG_MB_CONTROL_FAKE_POLICY_BLOCKED", "false");

    stream
        << "{\n"
        << "  \"source\": \"fake-bench\",\n"
        << "  \"command\": \"" << command << "\",\n"
        << "  \"mode\": \"" << mode << "\",\n"
        << "  \"duration_ms\": " << duration_ms << ",\n"
        << "  \"sample\": " << sample_index << ",\n"
        << "  \"snapshot_time\": \"" << snapshot_time_iso << "\",\n"
        << "  \"policy_writes_enabled\": " << policy_writes_enabled << ",\n"
        << "  \"policy_restore_on_exit\": " << policy_restore_on_exit << ",\n"
        << "  \"fans\": [\n"
        << "    {\n"
        << "      \"channel\": " << fan_channel << ",\n"
        << "      \"duty_raw\": " << fan_duty_raw << ",\n"
        << "      \"mode_raw\": " << fan_mode_raw << ",\n"
        << "      \"write_allowed\": " << write_allowed << ",\n"
        << "      \"policy_blocked\": " << policy_blocked << ",\n"
        << "      \"effective_write_allowed\": " << effective_write << "\n"
        << "    }\n"
        << "  ]\n"
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
        if (command != "read-snapshot" && command != "logger-service" &&
            command != "set-fixed-duty" && command != "restore-auto") {
            std::cerr << "fake-bench unsupported command: " << command << '\n';
            return 2;
        }

        if (command == "set-fixed-duty") {
            unsigned long channel = 0ul;
            double pct = 0.0;
            unsigned long hold_ms = 0ul;
            for (int index = 2; index < argc; ++index) {
                const std::string arg = argv[index];
                if (arg == "--channel" && index + 1 < argc) {
                    channel = std::stoul(argv[++index]);
                } else if (arg == "--pct" && index + 1 < argc) {
                    pct = std::stod(argv[++index]);
                } else if (arg == "--hold-ms" && index + 1 < argc) {
                    hold_ms = std::stoul(argv[++index]);
                }
            }
            const std::string write_mode = env_value(
                "SVG_MB_CONTROL_FAKE_WRITE_MODE", "default");
            if (write_mode == "fail_immediate") {
                std::cerr << "simulated set-fixed-duty failure\n";
                return 13;
            }
            if (write_mode == "policy_refused") {
                std::cerr << "runtime policy disables write commands\n";
                return 2;
            }
            if (hold_ms > 0ul) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(hold_ms));
            } else {
                // hold_ms == 0 means run until CTRL_BREAK. Default handler
                // on Windows terminates the process; we simulate that by
                // sleeping for a long time.
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
            std::cout << "set-fixed-duty\n";
            std::cout << "channel: " << channel << '\n';
            std::cout << "target_pct: " << pct << '\n';
            std::cout << "hold_ms: " << hold_ms << '\n';
            std::cout << "baseline_duty_raw: 128\n";
            std::cout << "baseline_mode_raw: 5\n";
            std::cout << "manifest_archive: fake_set_fixed_duty_manifest.json\n";
            return 0;
        }

        if (command == "restore-auto") {
            unsigned long channel = 0ul;
            unsigned long saved_duty_raw = 0ul;
            unsigned long saved_mode_raw = 0ul;
            for (int index = 2; index < argc; ++index) {
                const std::string arg = argv[index];
                if (arg == "--channel" && index + 1 < argc) {
                    channel = std::stoul(argv[++index]);
                } else if (arg == "--saved-duty-raw" && index + 1 < argc) {
                    saved_duty_raw = std::stoul(argv[++index]);
                } else if (arg == "--saved-mode-raw" && index + 1 < argc) {
                    saved_mode_raw = std::stoul(argv[++index]);
                }
            }
            const std::string restore_mode = env_value(
                "SVG_MB_CONTROL_FAKE_RESTORE_MODE", "success");
            if (restore_mode == "fail") {
                std::cerr << "simulated restore-auto failure\n";
                return 14;
            }
            std::cout << "restore-auto\n";
            std::cout << "channel: " << channel << '\n';
            std::cout << "saved_duty_raw: " << saved_duty_raw << '\n';
            std::cout << "saved_mode_raw: " << saved_mode_raw << '\n';
            std::cout << "manifest_archive: fake_restore_auto_manifest.json\n";
            return 0;
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
