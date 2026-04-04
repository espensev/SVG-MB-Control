#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path executable_directory(const char* argv0) {
    return std::filesystem::absolute(std::filesystem::path(argv0)).parent_path();
}

std::string fake_mode() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "SVG_MB_CONTROL_FAKE_MODE") == 0 &&
        value != nullptr) {
        std::string mode(value);
        free(value);
        return mode;
    }
    return "success";
}

void write_fake_snapshot(const std::filesystem::path& path,
                         const std::string& command,
                         const std::string& mode,
                         unsigned long duration_ms) {
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
        << "  \"sample\": 1\n"
        << "}\n";
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

        if (mode == "success") {
            write_fake_snapshot(snapshot_path, command, mode, duration_ms);
        } else if (mode == "missing_snapshot_file") {
            std::error_code ec;
            std::filesystem::remove(snapshot_path, ec);
        } else if (mode == "missing_snapshot_path") {
            write_fake_snapshot(snapshot_path, command, mode, duration_ms);
        } else {
            write_fake_snapshot(snapshot_path, command, mode, duration_ms);
        }

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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fake-bench error: " << error.what() << '\n';
        return 9;
    }
}
