#include "bench_bridge.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control [--bench-exe-path <path>] [--timeout-ms <ms>]\n"
        << "  svg-mb-control --help|-h\n"
        << "  svg-mb-control --version\n";
}

void PrintVersion() {
    std::cout << "svg-mb-control " << kVersion;
    if (std::string(kGitHash) != "unknown") {
        std::cout << " (" << kGitHash << ")";
    }
    std::cout << '\n';
}

std::uint32_t ParseTimeoutMs(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        if (parsed == 0ul) {
            throw std::runtime_error("timeout must be greater than zero");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --timeout-ms value.");
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path bench_exe_path;
        std::uint32_t timeout_ms = 15000u;

        for (int index = 1; index < argc; ++index) {
            const std::wstring arg = argv[index];
            auto require_value = [&]() -> const wchar_t* {
                if (index + 1 >= argc) {
                    throw std::runtime_error("Missing value for option.");
                }
                ++index;
                return argv[index];
            };

            if (arg == L"--bench-exe-path") {
                bench_exe_path = std::filesystem::path(require_value());
            } else if (arg == L"--timeout-ms") {
                timeout_ms = ParseTimeoutMs(require_value());
            } else if (arg == L"--help" || arg == L"-h") {
                PrintUsage();
                return 0;
            } else if (arg == L"--version") {
                PrintVersion();
                return 0;
            } else {
                throw std::runtime_error("Unknown option.");
            }
        }

        if (bench_exe_path.empty()) {
            bench_exe_path = svg_mb_control::ResolveDefaultBenchExecutablePath();
        }
        if (bench_exe_path.empty()) {
            throw std::runtime_error("Could not resolve svg-mb-bench.exe. Pass --bench-exe-path.");
        }

        const auto launch = svg_mb_control::RunReadSnapshot(bench_exe_path.wstring(),
                                                            timeout_ms);
        const std::string snapshot_json =
            svg_mb_control::LoadSnapshotJson(launch.snapshot_archive_path);

        std::cout << snapshot_json;
        if (snapshot_json.empty() || snapshot_json.back() != '\n') {
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
