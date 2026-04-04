#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
                         const std::string& mode) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open fake snapshot file.");
    }

    stream
        << "{\n"
        << "  \"source\": \"fake-bench\",\n"
        << "  \"mode\": \"" << mode << "\",\n"
        << "  \"sample\": 1\n"
        << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) != "read-snapshot") {
            std::cerr << "fake-bench requires read-snapshot\n";
            return 2;
        }

        const std::string mode = fake_mode();
        if (mode == "fail_exit") {
            std::cerr << "simulated read-snapshot failure\n";
            return 7;
        }

        const std::filesystem::path snapshot_path =
            executable_directory(argv[0]) / "fake_read_snapshot_snapshot.json";

        if (mode == "success") {
            write_fake_snapshot(snapshot_path, mode);
        } else if (mode == "missing_snapshot_file") {
            std::error_code ec;
            std::filesystem::remove(snapshot_path, ec);
        } else if (mode == "missing_snapshot_path") {
            write_fake_snapshot(snapshot_path, mode);
        } else {
            write_fake_snapshot(snapshot_path, mode);
        }

        std::cout << "read-snapshot\n";
        std::cout << "runtime_policy_path: (defaults)\n";
        std::cout << "amd_capture: 1\n";
        std::cout << "sio_capture: 1\n";
        if (mode != "missing_snapshot_path") {
            std::cout << "snapshot_archive: " << snapshot_path.string() << '\n';
        }
        std::cout << "manifest_archive: fake_manifest.json\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fake-bench error: " << error.what() << '\n';
        return 9;
    }
}
