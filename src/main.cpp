#include "amd_reader.h"
#include "analyze/analyze_ingest.h"
#include "calibration.h"
#include "control_config.h"
#include "control_loop.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "read_loop.h"
#include "runtime_lifecycle.h"
#include "runtime_logging.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"
#include "write_orchestrator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr const char* kVersion = SVG_MB_CONTROL_VERSION;
constexpr const char* kGitHash = SVG_MB_CONTROL_GIT_HASH;

enum class RunMode {
    kOneShot,
    kReadLoop,
    kWriteOnce,
    kControlLoop,
    kCalibrate,
};

svg_mb_control::ReadLoop* g_active_read_loop = nullptr;
std::atomic<bool> g_stop_signaled{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop_signaled.store(true);
            if (g_active_read_loop != nullptr) {
                g_active_read_loop->RequestStop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control [--start|--status|--stop|--restart] [--config <path>]\n"
        << "  svg-mb-control [--mode <one-shot|read-loop|write-once|control-loop|calibrate>] [--config <path>] "
           << "[--write-channel <n>] [--write-pct <pct>] [--write-hold-ms <ms>]\n"
        << "  svg-mb-control --mode calibrate [--calibrate-channel <n>] "
           << "[--calibrate-step-ms <ms>] [--calibrate-cooldown-ms <ms>] "
           << "[--calibrate-settle-window-ms <ms>] [--calibrate-abort-temp-c <c>] "
           << "[--calibrate-output <path>]\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "  svg-mb-control --diagnose-amd\n"
        << "  svg-mb-control --diagnose-gpu\n"
        << "  svg-mb-control --confirm-start\n"
        << "  svg-mb-control --help|-h\n"
        << "  svg-mb-control --version\n";
}

void PrintAnalyzeUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "    Reads CSV archives, manifests, events.jsonl and "
           << "plant_model.json from the\n"
        << "    runtime home and ingests them into a sqlite database. "
           << "Default db path is\n"
        << "    <runtime-home>/svg_mb_control.db. Idempotent on "
           << "previously-seen artifacts\n"
        << "    unless --force is passed.\n";
}

int RunAnalyzeCommand(int argc, wchar_t** argv) {
    if (argc < 3) {
        PrintAnalyzeUsage();
        return 1;
    }
    const std::wstring verb = argv[2];
    if (verb == L"--help" || verb == L"-h") {
        PrintAnalyzeUsage();
        return 0;
    }
    if (verb != L"ingest") {
        std::cerr << "Error: unknown analyze subcommand. Try "
                  << "'svg-mb-control analyze --help'.\n";
        return 1;
    }

    svg_mb_control::analyze::IngestOptions options;
    std::filesystem::path config_path;
    bool config_path_explicit = false;

    auto require_value = [&](int& index) -> const wchar_t* {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for option.");
        }
        ++index;
        return argv[index];
    };

    for (int index = 3; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--runtime-home") {
            options.runtime_home = std::filesystem::path(require_value(index));
        } else if (arg == L"--db") {
            options.db_path = std::filesystem::path(require_value(index));
        } else if (arg == L"--config") {
            config_path = std::filesystem::path(require_value(index));
            config_path_explicit = true;
        } else if (arg == L"--force") {
            options.force = true;
        } else if (arg == L"--quiet") {
            options.quiet = true;
        } else if (arg == L"--help" || arg == L"-h") {
            PrintAnalyzeUsage();
            return 0;
        } else {
            std::cerr << "Error: unknown analyze ingest option.\n";
            PrintAnalyzeUsage();
            return 1;
        }
    }

    if (options.runtime_home.empty()) {
        if (config_path.empty()) {
            config_path = svg_mb_control::GetEnvironmentPath(
                L"SVG_MB_CONTROL_CONFIG");
        }
        if (config_path.empty()) {
            config_path = svg_mb_control::ResolveDefaultControlConfigPath();
        }
        std::optional<svg_mb_control::ControlConfig> config;
        if (!config_path.empty()) {
            const std::filesystem::path absolute_config_path =
                std::filesystem::absolute(config_path).lexically_normal();
            std::error_code ec;
            if (std::filesystem::exists(absolute_config_path, ec)) {
                try {
                    config = svg_mb_control::LoadControlConfig(
                        absolute_config_path);
                } catch (const std::exception&) {
                    config.reset();
                }
            } else if (config_path_explicit) {
                std::cerr << "Error: control config not found: "
                          << absolute_config_path.string() << '\n';
                return 1;
            }
        }
        options.runtime_home = config.has_value()
            ? svg_mb_control::ResolveRuntimeHomePath(*config)
            : svg_mb_control::ResolveRuntimeHomePath(
                  svg_mb_control::ControlConfig{});
    }

    return svg_mb_control::analyze::RunAnalyzeIngest(options);
}

void PrintVersion() {
    std::cout << "svg-mb-control " << kVersion;
    if (std::string(kGitHash) != "unknown") {
        std::cout << " (" << kGitHash << ")";
    }
    std::cout << '\n';
}

bool IsLongRunningMode(RunMode mode) {
    return mode == RunMode::kReadLoop || mode == RunMode::kControlLoop;
}

std::wstring RunModeArgument(RunMode mode) {
    switch (mode) {
        case RunMode::kOneShot:
            return L"one-shot";
        case RunMode::kReadLoop:
            return L"read-loop";
        case RunMode::kWriteOnce:
            return L"write-once";
        case RunMode::kControlLoop:
            return L"control-loop";
        case RunMode::kCalibrate:
            return L"calibrate";
    }
    throw std::runtime_error("Unknown run mode.");
}

std::string_view RunModeLabel(RunMode mode) {
    switch (mode) {
        case RunMode::kOneShot:
            return "one-shot";
        case RunMode::kReadLoop:
            return "read-loop";
        case RunMode::kWriteOnce:
            return "write-once";
        case RunMode::kControlLoop:
            return "control-loop";
        case RunMode::kCalibrate:
            return "calibrate";
    }
    throw std::runtime_error("Unknown run mode.");
}

std::filesystem::path CurrentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(buffer.data(),
                                         buffer.data() + length);
        }
        buffer.resize(buffer.size() * 2u);
    }
}

std::wstring QuoteCommandLineArg(std::wstring_view value) {
    std::wstring quoted;
    quoted.reserve(value.size() + 2u);
    quoted.push_back(L'"');
    std::size_t backslashes = 0u;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2u + 1u, L'\\');
            quoted.push_back(ch);
            backslashes = 0u;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0u;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2u, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

HANDLE OpenInheritedFile(const std::filesystem::path& path,
                         DWORD desired_access,
                         DWORD creation_disposition) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;
    return CreateFileW(path.wstring().c_str(),
                       desired_access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       &security_attributes,
                       creation_disposition,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

std::string ReadTextFileTail(const std::filesystem::path& path,
                             std::uintmax_t max_bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }

    stream.seekg(0, std::ios::end);
    const std::ifstream::pos_type end = stream.tellg();
    if (end <= 0) {
        return {};
    }

    const auto length = static_cast<std::uintmax_t>(end);
    const auto start = length > max_bytes ? length - max_bytes : 0u;
    stream.seekg(static_cast<std::streamoff>(start), std::ios::beg);

    std::string content;
    content.assign(std::istreambuf_iterator<char>(stream),
                   std::istreambuf_iterator<char>());
    return content;
}

std::optional<nlohmann::json> TryReadJsonObject(
    const std::filesystem::path& path,
    std::string_view contract_name) {
    try {
        nlohmann::json payload = svg_mb_control::ReadJsonFile(
            path, contract_name);
        if (payload.is_object()) {
            return payload;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::string JsonStringOr(const nlohmann::json& value,
                         std::string_view key,
                         std::string_view fallback = {}) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::string(fallback);
}

std::uint32_t JsonUInt32Or(const nlohmann::json& value,
                           std::string_view key,
                           std::uint32_t fallback = 0u) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_number_unsigned()) {
        return found->get<std::uint32_t>();
    }
    if (found != value.end() && found->is_number_integer()) {
        const auto raw = found->get<std::int64_t>();
        if (raw > 0 && raw <= static_cast<std::int64_t>(UINT32_MAX)) {
            return static_cast<std::uint32_t>(raw);
        }
    }
    return fallback;
}

bool IsProcessActive(std::uint32_t pid) {
    if (pid == 0u) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    DWORD exit_code = 0u;
    const bool active =
        GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return active;
}

std::filesystem::path RuntimeStatusPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "control_runtime.json";
}

bool RuntimeStatusLooksActive(const nlohmann::json& status) {
    const std::string state = JsonStringOr(status, "status");
    if (state == "shutdown" || state == "failed" ||
        state == "direct-read-failed") {
        return false;
    }
    const std::uint32_t pid = JsonUInt32Or(status, "process_id");
    return pid != 0u && IsProcessActive(pid);
}

bool PrintAlreadyRunningIfActive(RunMode mode,
                                 const std::filesystem::path& runtime_home) {
    const auto status = TryReadJsonObject(
        RuntimeStatusPath(runtime_home), "runtime status");
    if (!status.has_value() || !RuntimeStatusLooksActive(*status)) {
        return false;
    }

    const std::uint32_t pid = JsonUInt32Or(*status, "process_id");
    const std::string active_mode =
        JsonStringOr(*status, "mode", RunModeLabel(mode));
    std::cout << "svg-mb-control: " << active_mode
              << " is already running\n"
              << "  pid: " << pid << '\n'
              << "  requested_mode: " << RunModeLabel(mode) << '\n'
              << "  status: " << RuntimeStatusPath(runtime_home).string()
              << '\n'
              << "  runtime_home: " << runtime_home.string() << '\n';
    return true;
}

int PrintRuntimeStatus(const std::filesystem::path& runtime_home) {
    const std::filesystem::path status_path = RuntimeStatusPath(runtime_home);
    const auto status = TryReadJsonObject(status_path, "runtime status");
    if (!status.has_value()) {
        std::cout << "svg-mb-control: not running\n"
                  << "  runtime_home: " << runtime_home.string() << '\n'
                  << "  status: " << status_path.string()
                  << " (not found)\n";
        return 0;
    }

    const std::string mode = JsonStringOr(*status, "mode", "(unknown)");
    const std::string state = JsonStringOr(*status, "status", "(unknown)");
    const std::string detail = JsonStringOr(*status, "status_detail");
    const std::string last_eval =
        JsonStringOr(*status, "loop_last_evaluation",
                     JsonStringOr(*status, "last_refresh"));
    const std::uint32_t pid = JsonUInt32Or(*status, "process_id");
    const bool active = RuntimeStatusLooksActive(*status);

    std::cout << "svg-mb-control: "
              << (active ? "running" : "not running") << '\n'
              << "  mode: " << mode << '\n'
              << "  status: " << state << '\n';
    if (!detail.empty()) {
        std::cout << "  detail: " << detail << '\n';
    }
    if (pid != 0u) {
        std::cout << "  pid: " << pid
                  << (active ? " (active)" : " (not active)") << '\n';
    }
    if (!last_eval.empty()) {
        std::cout << "  last_update: " << last_eval << '\n';
    }
    std::cout << "  runtime_home: " << runtime_home.string() << '\n'
              << "  status_file: " << status_path.string() << '\n';
    const std::string log_csv = JsonStringOr(*status, "log_csv_path");
    const std::string event_log = JsonStringOr(*status, "event_log_path");
    if (!log_csv.empty()) {
        std::cout << "  csv: " << log_csv << '\n';
    }
    if (!event_log.empty()) {
        std::cout << "  events: " << event_log << '\n';
    }
    return 0;
}

bool WaitForRuntimeStop(const std::filesystem::path& runtime_home,
                        std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = TryReadJsonObject(
            RuntimeStatusPath(runtime_home), "runtime status");
        if (!status.has_value() || !RuntimeStatusLooksActive(*status)) {
            return true;
        }
        const std::string state = JsonStringOr(*status, "status");
        if (state == "shutdown") {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int RequestStopAndWait(const std::filesystem::path& runtime_home,
                       bool quiet = false) {
    if (!svg_mb_control::RequestRuntimeStop(runtime_home)) {
        std::cerr << "Error: could not write stop request under "
                  << runtime_home.string() << '\n';
        return 1;
    }
    if (!quiet) {
        std::cout << "svg-mb-control: stop requested\n"
                  << "  runtime_home: " << runtime_home.string() << '\n'
                  << "  stop_request: "
                  << svg_mb_control::RuntimeStopRequestPath(runtime_home).string()
                  << '\n';
    }
    if (!WaitForRuntimeStop(runtime_home, std::chrono::seconds(15))) {
        std::cerr << "Warning: controller did not report stopped within 15s.\n"
                  << "  status: " << RuntimeStatusPath(runtime_home).string()
                  << '\n';
        return 2;
    }
    if (!quiet) {
        std::cout << "svg-mb-control: stopped\n";
    }
    return 0;
}

std::wstring BuildManagedCommandLine(const std::filesystem::path& exe_path,
                                     RunMode mode,
                                     const std::filesystem::path& config_path,
                                     bool supervisor) {
    std::wstring command_line = QuoteCommandLineArg(exe_path.wstring());
    command_line += supervisor ? L" --run-supervisor" : L" --run-foreground";
    command_line += L" --mode ";
    command_line += RunModeArgument(mode);
    command_line += L" --config ";
    command_line += QuoteCommandLineArg(config_path.wstring());
    return command_line;
}

struct StartedProcess {
    DWORD pid = 0u;
    HANDLE process_handle = nullptr;
};

StartedProcess StartHiddenProcess(
    const std::filesystem::path& exe_path,
    const std::wstring& command_line,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path) {
    HANDLE stdout_handle =
        OpenInheritedFile(stdout_path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (stdout_handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not open launcher stdout log.");
    }
    HANDLE stderr_handle =
        OpenInheritedFile(stderr_path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_handle);
        throw std::runtime_error("Could not open launcher stderr log.");
    }
    HANDLE stdin_handle =
        OpenInheritedFile("NUL", GENERIC_READ, OPEN_EXISTING);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_handle);
        CloseHandle(stderr_handle);
        throw std::runtime_error("Could not open launcher stdin handle.");
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_handle;
    startup_info.hStdOutput = stdout_handle;
    startup_info.hStdError = stderr_handle;

    std::array<HANDLE, 3> inherited_handles{
        stdin_handle,
        stdout_handle,
        stderr_handle,
    };
    SIZE_T attribute_list_size = 0u;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
    std::vector<unsigned char> attribute_list_storage(attribute_list_size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_list_storage.data());
    if (!InitializeProcThreadAttributeList(
            attribute_list, 1, 0, &attribute_list_size)) {
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        CloseHandle(stderr_handle);
        throw std::runtime_error("Could not initialize process attributes.");
    }
    if (!UpdateProcThreadAttribute(
            attribute_list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            sizeof(HANDLE) * inherited_handles.size(),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        CloseHandle(stderr_handle);
        throw std::runtime_error("Could not configure child handle inheritance.");
    }

    STARTUPINFOEXW startup_info_ex{};
    startup_info_ex.StartupInfo = startup_info;
    startup_info_ex.StartupInfo.cb = sizeof(startup_info_ex);
    startup_info_ex.lpAttributeList = attribute_list;

    PROCESS_INFORMATION process_info{};
    std::wstring mutable_command_line = command_line;
    const BOOL created = CreateProcessW(
        exe_path.wstring().c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP |
            CREATE_NO_WINDOW,
        nullptr,
        working_directory.wstring().c_str(),
        &startup_info_ex.StartupInfo,
        &process_info);

    DeleteProcThreadAttributeList(attribute_list);
    CloseHandle(stdin_handle);
    CloseHandle(stdout_handle);
    CloseHandle(stderr_handle);

    if (!created) {
        throw std::runtime_error("Could not launch background controller.");
    }

    CloseHandle(process_info.hThread);
    return StartedProcess{process_info.dwProcessId, process_info.hProcess};
}

bool ConfirmDetachedLaunch(RunMode mode,
                           const svg_mb_control::ControlConfig& config) {
    const std::filesystem::path runtime_home =
        svg_mb_control::ResolveRuntimeHomePath(config);
    std::wstring message =
        L"Start SVG-MB Control in the background?\n\nMode: ";
    message += RunModeArgument(mode);
    message += L"\n\nConfig:\n";
    message += config.source_path.wstring();
    message += L"\n\nRuntime:\n";
    message += runtime_home.wstring();

    const int result = MessageBoxW(nullptr,
                                   message.c_str(),
                                   L"SVG-MB Control",
                                   MB_YESNO | MB_DEFBUTTON2 |
                                       MB_ICONQUESTION | MB_SETFOREGROUND);
    return result == IDYES;
}

int LaunchDetachedLongRunningMode(RunMode mode,
                                  const svg_mb_control::ControlConfig& config) {
    const std::filesystem::path exe_path = CurrentExecutablePath();
    if (exe_path.empty()) {
        throw std::runtime_error("Could not resolve current executable path.");
    }

    const std::filesystem::path runtime_home =
        svg_mb_control::ResolveRuntimeHomePath(config);
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) {
        throw std::runtime_error("Could not create runtime home: " +
                                 ec.message());
    }
    if (PrintAlreadyRunningIfActive(mode, runtime_home)) {
        return 0;
    }
    svg_mb_control::ClearRuntimeStopRequest(runtime_home);

    const std::filesystem::path stdout_path =
        runtime_home / "svg-mb-control.supervisor.stdout.log";
    const std::filesystem::path stderr_path =
        runtime_home / "svg-mb-control.supervisor.stderr.log";
    const std::filesystem::path working_directory = exe_path.parent_path();
    StartedProcess supervisor = StartHiddenProcess(
        exe_path,
        BuildManagedCommandLine(exe_path, mode, config.source_path, true),
        working_directory,
        stdout_path,
        stderr_path);

    constexpr DWORD kStartupCheckMs = 1500u;
    const DWORD startup_wait =
        WaitForSingleObject(supervisor.process_handle, kStartupCheckMs);
    if (startup_wait == WAIT_OBJECT_0) {
        DWORD exit_code = 1u;
        GetExitCodeProcess(supervisor.process_handle, &exit_code);
        CloseHandle(supervisor.process_handle);

        std::cerr << "Error: "
                  << RunModeLabel(mode)
                  << " background supervisor exited during startup"
                  << " (pid: " << supervisor.pid
                  << ", exit_code: " << exit_code << ").\n"
                  << "  stderr: " << stderr_path.string() << '\n';
        const std::string stderr_tail =
            ReadTextFileTail(stderr_path, 4096u);
        if (!stderr_tail.empty()) {
            std::cerr << "\n--- stderr tail ---\n"
                      << stderr_tail
                      << "\n--- end stderr tail ---\n";
        }
        return exit_code == 0u ? 1 : static_cast<int>(exit_code);
    }
    CloseHandle(supervisor.process_handle);

    std::cout << "svg-mb-control: launched "
              << RunModeLabel(mode)
              << " in background\n"
              << "  supervisor_pid: " << supervisor.pid << '\n'
              << "  config: " << config.source_path.string() << '\n'
              << "  runtime_home: " << runtime_home.string() << '\n'
              << "  status: " << (runtime_home / "control_runtime.json").string()
              << '\n'
              << "  supervisor_stdout: " << stdout_path.string() << '\n'
              << "  supervisor_stderr: " << stderr_path.string() << '\n'
              << "  worker_stdout: "
              << (runtime_home / "svg-mb-control.worker.stdout.log").string()
              << '\n'
              << "  worker_stderr: "
              << (runtime_home / "svg-mb-control.worker.stderr.log").string()
              << '\n';
    return 0;
}

int RunSupervisedLongRunningMode(RunMode mode,
                                 const svg_mb_control::ControlConfig& config) {
    const std::filesystem::path exe_path = CurrentExecutablePath();
    if (exe_path.empty()) {
        throw std::runtime_error("Could not resolve current executable path.");
    }
    const std::filesystem::path runtime_home =
        svg_mb_control::ResolveRuntimeHomePath(config);
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    if (ec) {
        throw std::runtime_error("Could not create runtime home: " +
                                 ec.message());
    }
    svg_mb_control::ClearRuntimeStopRequest(runtime_home);

    const std::filesystem::path working_directory = exe_path.parent_path();
    const std::filesystem::path stdout_path =
        runtime_home / "svg-mb-control.worker.stdout.log";
    const std::filesystem::path stderr_path =
        runtime_home / "svg-mb-control.worker.stderr.log";
    std::uint32_t restart_count = 0u;

    svg_mb_control::AppendRuntimeEvent(
        runtime_home,
        svg_mb_control::RuntimeLogEvent{
            .mode = std::string(RunModeLabel(mode)),
            .event_type = "supervisor.start",
            .detail = "background supervisor started",
            .success = true,
        });

    while (!svg_mb_control::RuntimeStopRequested(runtime_home)) {
        StartedProcess worker = StartHiddenProcess(
            exe_path,
            BuildManagedCommandLine(exe_path, mode, config.source_path, false),
            working_directory,
            stdout_path,
            stderr_path);
        {
            std::ostringstream detail;
            detail << "worker started pid=" << worker.pid
                   << " restart_count=" << restart_count;
            svg_mb_control::AppendRuntimeEvent(
                runtime_home,
                svg_mb_control::RuntimeLogEvent{
                    .mode = std::string(RunModeLabel(mode)),
                    .event_type = "supervisor.worker_started",
                    .detail = detail.str(),
                    .success = true,
                });
        }

        constexpr DWORD kWorkerStartupCheckMs = 1500u;
        DWORD wait_result =
            WaitForSingleObject(worker.process_handle, kWorkerStartupCheckMs);
        const bool exited_during_startup = wait_result == WAIT_OBJECT_0;
        DWORD exit_code = STILL_ACTIVE;
        if (exited_during_startup) {
            GetExitCodeProcess(worker.process_handle, &exit_code);
        } else {
            while (wait_result == WAIT_TIMEOUT) {
                wait_result = WaitForSingleObject(worker.process_handle, 1000u);
            }
            GetExitCodeProcess(worker.process_handle, &exit_code);
        }

        const bool stop_requested =
            svg_mb_control::RuntimeStopRequested(runtime_home);
        {
            std::ostringstream detail;
            detail << "worker exited pid=" << worker.pid
                   << " exit_code=" << exit_code
                   << " stop_requested="
                   << (stop_requested ? "true" : "false");
            svg_mb_control::AppendRuntimeEvent(
                runtime_home,
                svg_mb_control::RuntimeLogEvent{
                    .mode = std::string(RunModeLabel(mode)),
                    .event_type = "supervisor.worker_exited",
                    .detail = detail.str(),
                    .success = exit_code == 0u,
                });
        }

        if (restart_count == 0u && exited_during_startup &&
            !stop_requested && exit_code != 0u) {
            std::cerr << "Error: " << RunModeLabel(mode)
                      << " worker exited during startup"
                      << " (pid: " << worker.pid
                      << ", exit_code: " << exit_code << ").\n"
                      << "  stderr: " << stderr_path.string() << '\n';
            const std::string stderr_tail =
                ReadTextFileTail(stderr_path, 4096u);
            if (!stderr_tail.empty()) {
                std::cerr << "\n--- worker stderr tail ---\n"
                          << stderr_tail
                          << "\n--- end worker stderr tail ---\n";
            }
            CloseHandle(worker.process_handle);
            return static_cast<int>(exit_code);
        }

        CloseHandle(worker.process_handle);

        if (stop_requested || exit_code == 0u) {
            break;
        }

        ++restart_count;
        const std::uint32_t backoff_seconds =
            (std::min)(60u, 1u << (std::min)(restart_count, 5u));
        {
            std::ostringstream detail;
            detail << "restarting worker after " << backoff_seconds
                   << "s; restart_count=" << restart_count;
            svg_mb_control::AppendRuntimeEvent(
                runtime_home,
                svg_mb_control::RuntimeLogEvent{
                    .mode = std::string(RunModeLabel(mode)),
                    .event_type = "supervisor.worker_restart_scheduled",
                    .detail = detail.str(),
                    .success = false,
                });
        }
        for (std::uint32_t elapsed = 0u;
             elapsed < backoff_seconds &&
             !svg_mb_control::RuntimeStopRequested(runtime_home);
             ++elapsed) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    svg_mb_control::AppendRuntimeEvent(
        runtime_home,
        svg_mb_control::RuntimeLogEvent{
            .mode = std::string(RunModeLabel(mode)),
            .event_type = "supervisor.shutdown",
            .detail = "background supervisor stopped",
            .success = true,
        });
    return 0;
}

void PrintBlockedChannels(const std::vector<std::uint32_t>& channels) {
    if (channels.empty()) {
        std::cout << "(none)";
        return;
    }

    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        std::cout << channels[index];
    }
}

void PrintCommonLoopStartup(const char* mode,
                            const svg_mb_control::ControlConfig& config,
                            const std::filesystem::path& runtime_home,
                            const svg_mb_control::RuntimeWritePolicy& policy) {
    std::cout << "svg-mb-control: starting " << mode << '\n'
              << "  pid: " << GetCurrentProcessId() << '\n'
              << "  config: " << config.source_path.string() << '\n'
              << "  runtime_home: " << runtime_home.string() << '\n'
              << "  status: " << (runtime_home / "control_runtime.json").string()
              << '\n'
              << "  events: "
              << (runtime_home / "logs" / "svg_mb_control_events.jsonl").string()
              << '\n'
              << "  policy: "
              << (policy.present ? policy.source_path.string()
                                 : std::string("(none)"))
              << '\n'
              << "  writes_enabled: "
              << (policy.writes_enabled ? "true" : "false") << '\n'
              << "  blocked_channels: ";
    PrintBlockedChannels(policy.blocked_channels);
    std::cout << '\n';
}

void PrintControlLoopStartup(
    const svg_mb_control::ControlConfig& config,
    const svg_mb_control::ControlLoopConfig& loop_config,
    const std::filesystem::path& runtime_home,
    const svg_mb_control::RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("control-loop", config, runtime_home, policy);
    std::cout << "  poll_tick_ms: " << loop_config.poll_tick_ms << '\n'
              << "  write_cooldown_ms: " << loop_config.write_cooldown_ms << '\n'
              << "  deadband_pct: " << loop_config.deadband_pct << '\n'
              << "  hold_ms: " << loop_config.control_hold_ms << '\n'
              << "  controlled_channels:\n";
    for (const auto& channel : loop_config.channels) {
        std::cout << "    channel " << channel.channel
                  << ": floor=" << channel.min_duty_pct
                  << "% blend="
                  << svg_mb_control::TempBlendToString(channel.temp_blend)
                  << " shape="
                  << svg_mb_control::CurveShapeToString(channel.curve_shape)
                  << " rise_rate=" << channel.rise_rate_pct_per_min
                  << "%/min fall_rate=" << channel.fall_rate_pct_per_min
                  << "%/min"
                  << " demand_alpha="
                  << channel.demand_smoothing_rise_alpha
                  << "/" << channel.demand_smoothing_fall_alpha
                  << " decay_latch="
                  << channel.decay_latch_above_pct
                  << "%/" << channel.decay_latch_pct_per_min
                  << "%/min"
                  << " thermal_pressure="
                  << channel.thermal_pressure_start_c
                  << '-' << channel.thermal_pressure_full_c
                  << "C +" << channel.thermal_pressure_max_boost_pct
                  << "% @ " << channel.thermal_pressure_rise_pct_per_sec
                  << "%/s -" << channel.thermal_pressure_fall_pct_per_sec
                  << "%/s"
                  << " curve=";
        for (std::size_t index = 0; index < channel.curve.size(); ++index) {
            if (index > 0) {
                std::cout << ',';
            }
            std::cout << channel.curve[index].temp_c << "C:"
                      << channel.curve[index].duty_pct << '%';
        }
        if (!channel.cpu_override_curve.empty()) {
            std::cout << " cpu_override_curve=";
            for (std::size_t index = 0;
                 index < channel.cpu_override_curve.size(); ++index) {
                if (index > 0) {
                    std::cout << ',';
                }
                std::cout << channel.cpu_override_curve[index].temp_c << "C:"
                          << channel.cpu_override_curve[index].duty_pct << '%';
            }
        }
        std::cout << '\n';
    }
    std::cout << std::flush;
}

void PrintReadLoopStartup(const svg_mb_control::ControlConfig& config,
                          const std::filesystem::path& runtime_home,
                          const svg_mb_control::RuntimeWritePolicy& policy) {
    PrintCommonLoopStartup("read-loop", config, runtime_home, policy);
    std::cout << "  poll_ms: " << config.poll_ms << '\n' << std::flush;
}

RunMode ParseRunMode(const wchar_t* value) {
    const std::wstring raw(value);
    if (raw == L"one-shot") {
        return RunMode::kOneShot;
    }
    if (raw == L"read-loop") {
        return RunMode::kReadLoop;
    }
    if (raw == L"write-once") {
        return RunMode::kWriteOnce;
    }
    if (raw == L"control-loop") {
        return RunMode::kControlLoop;
    }
    if (raw == L"calibrate") {
        return RunMode::kCalibrate;
    }
    throw std::runtime_error("Invalid --mode value.");
}

RunMode ParseRunMode(std::string_view value) {
    if (value == "one-shot") {
        return RunMode::kOneShot;
    }
    if (value == "read-loop") {
        return RunMode::kReadLoop;
    }
    if (value == "write-once") {
        return RunMode::kWriteOnce;
    }
    if (value == "control-loop") {
        return RunMode::kControlLoop;
    }
    if (value == "calibrate") {
        return RunMode::kCalibrate;
    }
    throw std::runtime_error("Invalid default_mode in control config.");
}

std::uint32_t ParseWriteChannel(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-channel value.");
    }
}

double ParseWritePct(const wchar_t* value) {
    try {
        return std::stod(std::wstring(value));
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-pct value.");
    }
}

std::uint32_t ParseWriteHoldMs(const wchar_t* value) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid --write-hold-ms value.");
    }
}

std::uint32_t ParseUInt32Arg(const wchar_t* value, const char* flag_name) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(value));
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

double ParseDoubleArg(const wchar_t* value, const char* flag_name) {
    try {
        return std::stod(std::wstring(value));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + flag_name + " value.");
    }
}

std::string SampleDirectSnapshotJson(
    const svg_mb_control::ControlConfig* config) {
    const svg_mb_control::RuntimeWritePolicy runtime_policy =
        svg_mb_control::ResolveRuntimeWritePolicy(config);
    std::unique_ptr<svg_mb_control::FanWriter> writer =
        svg_mb_control::CreateFanWriter(runtime_policy);
    svg_mb_control::AmdReader amd_reader;
    svg_mb_control::GpuReader gpu_reader;
    const svg_mb_control::RuntimeSnapshot snapshot =
        svg_mb_control::SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *writer, runtime_policy);
    return svg_mb_control::SerializeRuntimeSnapshotJson(snapshot);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"analyze") {
            return RunAnalyzeCommand(argc, argv);
        }
        const bool no_launch_args = argc == 1;
        std::filesystem::path config_path;
        bool config_path_explicit = false;
        bool foreground_launch = false;
        bool supervisor_launch = false;
        bool confirm_start = false;
        bool start_requested = false;
        bool status_requested = false;
        bool stop_requested = false;
        bool restart_requested = false;
        RunMode run_mode = RunMode::kOneShot;
        bool run_mode_explicit = false;
        std::uint32_t write_channel = 0u;
        bool write_channel_explicit = false;
        double write_pct = 0.0;
        bool write_pct_explicit = false;
        std::uint32_t write_hold_ms = 0u;
        bool write_hold_ms_explicit = false;
        std::optional<std::uint32_t> calibrate_channel;
        std::optional<std::uint32_t> calibrate_step_ms;
        std::optional<std::uint32_t> calibrate_cooldown_ms;
        std::optional<std::uint32_t> calibrate_settle_window_ms;
        std::optional<double> calibrate_abort_temp_c;
        std::filesystem::path calibrate_output_path;

        for (int index = 1; index < argc; ++index) {
            const std::wstring arg = argv[index];
            auto require_value = [&]() -> const wchar_t* {
                if (index + 1 >= argc) {
                    throw std::runtime_error("Missing value for option.");
                }
                ++index;
                return argv[index];
            };

            if (arg == L"--config") {
                config_path = std::filesystem::path(require_value());
                config_path_explicit = true;
            } else if (arg == L"--run-foreground") {
                foreground_launch = true;
            } else if (arg == L"--run-supervisor") {
                supervisor_launch = true;
            } else if (arg == L"--start") {
                start_requested = true;
            } else if (arg == L"--status") {
                status_requested = true;
            } else if (arg == L"--stop") {
                stop_requested = true;
            } else if (arg == L"--restart") {
                restart_requested = true;
            } else if (arg == L"--confirm-start") {
                confirm_start = true;
            } else if (arg == L"--mode") {
                run_mode = ParseRunMode(require_value());
                run_mode_explicit = true;
            } else if (arg == L"--write-channel") {
                write_channel = ParseWriteChannel(require_value());
                write_channel_explicit = true;
            } else if (arg == L"--write-pct") {
                write_pct = ParseWritePct(require_value());
                write_pct_explicit = true;
            } else if (arg == L"--write-hold-ms") {
                write_hold_ms = ParseWriteHoldMs(require_value());
                write_hold_ms_explicit = true;
            } else if (arg == L"--calibrate-channel") {
                calibrate_channel = ParseUInt32Arg(
                    require_value(), "--calibrate-channel");
            } else if (arg == L"--calibrate-step-ms") {
                calibrate_step_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-step-ms");
            } else if (arg == L"--calibrate-cooldown-ms") {
                calibrate_cooldown_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-cooldown-ms");
            } else if (arg == L"--calibrate-settle-window-ms") {
                calibrate_settle_window_ms = ParseUInt32Arg(
                    require_value(), "--calibrate-settle-window-ms");
            } else if (arg == L"--calibrate-abort-temp-c") {
                calibrate_abort_temp_c = ParseDoubleArg(
                    require_value(), "--calibrate-abort-temp-c");
            } else if (arg == L"--calibrate-output") {
                calibrate_output_path =
                    std::filesystem::path(require_value());
            } else if (arg == L"--help" || arg == L"-h") {
                PrintUsage();
                return 0;
            } else if (arg == L"--version") {
                PrintVersion();
                return 0;
            } else if (arg == L"--bridge-exe-path" ||
                       arg == L"--bench-exe-path" ||
                       arg == L"--bridge-command" ||
                       arg == L"--duration-ms" ||
                       arg == L"--timeout-ms") {
                throw std::runtime_error(
                    "Legacy bridge options were removed. This branch runs direct-only.");
            } else if (arg == L"--diagnose-amd") {
                svg_mb_control::AmdReader reader;
                std::cout << "amd_reader.available: "
                          << (reader.available() ? "true" : "false") << '\n';
                std::cout << "amd_reader.init_warning: \""
                          << reader.init_warning() << "\"\n";
                const auto snapshot = reader.Sample();
                std::cout << "sample.available: "
                          << (snapshot.available ? "true" : "false") << '\n';
                std::cout << "sample.cpu_name: \"" << snapshot.cpu_name << "\"\n";
                std::cout << "sample.transport_path: \""
                          << snapshot.transport_path << "\"\n";
                std::cout << "sample.last_warning: \""
                          << snapshot.last_warning << "\"\n";
                std::cout << "sample.count: " << snapshot.samples.size() << '\n';
                for (std::size_t sample_index = 0u;
                     sample_index < snapshot.samples.size();
                     ++sample_index) {
                    const auto& sample = snapshot.samples[sample_index];
                    std::cout << "sample[" << sample_index << "].label: \""
                              << sample.label << "\"\n";
                    std::cout << "sample[" << sample_index
                              << "].temperature_c: " << sample.temperature_c
                              << '\n';
                }
                return snapshot.available ? 0 : 1;
            } else if (arg == L"--diagnose-gpu") {
                svg_mb_control::GpuReader reader;
                std::cout << "gpu_reader.available: "
                          << (reader.available() ? "true" : "false") << '\n';
                std::cout << "gpu_reader.init_warning: \""
                          << reader.init_warning() << "\"\n";
                const auto sample = reader.Sample();
                std::cout << "sample.available: "
                          << (sample.available ? "true" : "false") << '\n';
                std::cout << "sample.gpu_name: \"" << sample.gpu_name << "\"\n";
                std::cout << "sample.core_c: " << sample.core_c << '\n';
                std::cout << "sample.memjn_c: " << sample.memjn_c << '\n';
                std::cout << "sample.hotspot_c: " << sample.hotspot_c << '\n';
                std::cout << "sample.last_warning: \""
                          << sample.last_warning << "\"\n";
                return sample.available ? 0 : 1;
            } else {
                throw std::runtime_error("Unknown option.");
            }
        }

        if (config_path.empty()) {
            config_path = svg_mb_control::GetEnvironmentPath(L"SVG_MB_CONTROL_CONFIG");
            if (!config_path.empty()) {
                config_path_explicit = true;
            }
        }
        if (config_path.empty()) {
            config_path = svg_mb_control::ResolveDefaultControlConfigPath();
        }

        std::optional<svg_mb_control::ControlConfig> config;
        if (!config_path.empty()) {
            const std::filesystem::path absolute_config_path =
                std::filesystem::absolute(config_path).lexically_normal();
            if (!std::filesystem::exists(absolute_config_path)) {
                if (config_path_explicit) {
                    throw std::runtime_error("Control config not found: " +
                                             absolute_config_path.string());
                }
            } else {
                config = svg_mb_control::LoadControlConfig(absolute_config_path);
            }
        }

        if (!run_mode_explicit && config.has_value() &&
            !config->default_mode.empty()) {
            run_mode = ParseRunMode(config->default_mode);
        }

        const svg_mb_control::ControlConfig status_config =
            config.has_value() ? *config : svg_mb_control::ControlConfig{};
        const std::filesystem::path command_runtime_home =
            svg_mb_control::ResolveRuntimeHomePath(status_config);

        if (status_requested) {
            return PrintRuntimeStatus(command_runtime_home);
        }

        if (stop_requested && !restart_requested) {
            return RequestStopAndWait(command_runtime_home);
        }

        if (restart_requested) {
            const int stop_result =
                RequestStopAndWait(command_runtime_home, true);
            if (stop_result != 0) {
                return stop_result;
            }
            start_requested = true;
        }

        if (supervisor_launch) {
            if (!config.has_value()) {
                throw std::runtime_error(
                    "--run-supervisor requires a control config.");
            }
            if (!IsLongRunningMode(run_mode)) {
                throw std::runtime_error(
                    "--run-supervisor requires read-loop or control-loop.");
            }
            return RunSupervisedLongRunningMode(run_mode, *config);
        }

        if (!foreground_launch && config.has_value() &&
            IsLongRunningMode(run_mode) &&
            (no_launch_args || confirm_start || start_requested)) {
            if (confirm_start && !ConfirmDetachedLaunch(run_mode, *config)) {
                std::cout << "svg-mb-control: start cancelled\n";
                return 0;
            }
            return LaunchDetachedLongRunningMode(run_mode, *config);
        }

        if (start_requested || restart_requested) {
            throw std::runtime_error(
                "--start/--restart requires a control config whose mode is read-loop or control-loop.");
        }

        if (config.has_value() && !config->runtime_policy_path.empty()) {
            const DWORD existing = GetEnvironmentVariableW(
                L"SVG_MB_RUNTIME_POLICY", nullptr, 0);
            if (existing == 0) {
                SetEnvironmentVariableW(
                    L"SVG_MB_RUNTIME_POLICY",
                    config->runtime_policy_path.wstring().c_str());
            }
        }

        const std::uint32_t reconcile_timeout_ms =
            config.has_value() ? config->restore_timeout_ms : 5000u;
        const std::filesystem::path reconcile_runtime_home =
            config.has_value()
                ? svg_mb_control::ResolveRuntimeHomePath(*config)
                : svg_mb_control::ResolveRuntimeHomePath(
                      svg_mb_control::ControlConfig{});
        const int reconcile_result = svg_mb_control::ReconcilePendingWrites(
            reconcile_runtime_home,
            svg_mb_control::ResolveRuntimeWritePolicy(
                config.has_value() ? &*config : nullptr),
            reconcile_timeout_ms);
        if (reconcile_result != 0) {
            std::cerr << "Error: pending writes reconciliation failed. "
                      << "Refusing to proceed." << '\n';
            return reconcile_result;
        }

        if (run_mode == RunMode::kWriteOnce) {
            if (!config.has_value()) {
                svg_mb_control::ControlConfig defaults;
                config = defaults;
            }
            if (!write_channel_explicit && !config->write_channel_set) {
                throw std::runtime_error("--mode write-once requires --write-channel or write_channel in config.");
            }
            if (!write_pct_explicit && !config->write_target_pct_set) {
                throw std::runtime_error("--mode write-once requires --write-pct or write_target_pct in config.");
            }
            if (!write_hold_ms_explicit && !config->write_hold_ms_set) {
                throw std::runtime_error("--mode write-once requires --write-hold-ms or write_hold_ms in config.");
            }
            svg_mb_control::WriteRequest request;
            request.channel = write_channel_explicit
                ? write_channel : config->write_channel;
            request.target_pct = write_pct_explicit
                ? write_pct : config->write_target_pct;
            request.hold_ms = write_hold_ms_explicit
                ? write_hold_ms : config->write_hold_ms;

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const int result = svg_mb_control::RunWriteOnce(
                *config, reconcile_runtime_home, request, g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kControlLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode control-loop requires a control config.");
            }
            if (config_path.empty()) {
                throw std::runtime_error("--mode control-loop requires a resolvable config path.");
            }
            const svg_mb_control::ControlLoopConfig loop_config =
                svg_mb_control::LoadControlLoopConfig(
                    std::filesystem::absolute(config_path).lexically_normal());
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintControlLoopStartup(
                *config, loop_config, reconcile_runtime_home, runtime_policy);

            svg_mb_control::ControlLoop control_loop(
                *config, loop_config, reconcile_runtime_home);

            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const int result = control_loop.RunUntilStopped(g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kCalibrate) {
            svg_mb_control::CalibrationOptions options =
                svg_mb_control::DefaultCalibrationOptions();
            if (calibrate_step_ms.has_value()) {
                for (auto& step : options.sequence) {
                    step.hold_ms = *calibrate_step_ms;
                }
            }
            if (calibrate_cooldown_ms.has_value() &&
                !options.sequence.empty()) {
                options.sequence.back().hold_ms = *calibrate_cooldown_ms;
            }
            if (calibrate_settle_window_ms.has_value()) {
                options.settle_window_ms = *calibrate_settle_window_ms;
            }
            if (calibrate_abort_temp_c.has_value()) {
                options.abort_temp_ceiling_c = *calibrate_abort_temp_c;
            }
            if (calibrate_channel.has_value()) {
                options.only_channel = *calibrate_channel;
            }
            if (!calibrate_output_path.empty()) {
                options.output_path = calibrate_output_path;
            }
            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }
            const svg_mb_control::ControlConfig effective_config =
                config.has_value() ? *config
                                   : svg_mb_control::ControlConfig{};
            const int result = svg_mb_control::RunCalibration(
                effective_config, reconcile_runtime_home, options,
                g_stop_signaled);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            return result;
        }

        if (run_mode == RunMode::kReadLoop) {
            if (!config.has_value()) {
                throw std::runtime_error("--mode read-loop requires a control config.");
            }

            const std::filesystem::path runtime_home =
                svg_mb_control::ResolveRuntimeHomePath(*config);
            const svg_mb_control::RuntimeWritePolicy runtime_policy =
                svg_mb_control::ResolveRuntimeWritePolicy(&*config);
            PrintReadLoopStartup(*config, runtime_home, runtime_policy);

            svg_mb_control::ReadLoop loop(*config, runtime_home);
            g_active_read_loop = &loop;
            if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
                g_active_read_loop = nullptr;
                throw std::runtime_error("SetConsoleCtrlHandler failed.");
            }

            const int loop_exit = loop.RunUntilStopped();

            SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            g_active_read_loop = nullptr;
            return loop_exit;
        }

        const std::string snapshot_json = SampleDirectSnapshotJson(
            config.has_value() ? &*config : nullptr);
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
