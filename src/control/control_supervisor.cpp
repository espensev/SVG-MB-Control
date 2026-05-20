// Supervisor / launcher subsystem for long-running modes. Split out of
// main.cpp so the CLI dispatcher and the supervised lifecycle (status,
// stop/restart, detached launch, in-process supervisor with restart-backoff)
// are independent translation units. This layer composes the stop-request
// primitives in runtime_lifecycle.h.

#include "control_supervisor.h"

#include "control_config.h"
#include "control_scheduler.h"
#include "json_io.h"
#include "read_loop.h"
#include "runtime_artifacts.h"
#include "runtime_lifecycle.h"
#include "runtime_supervisor_state.h"
#include "runtime_util.h"

#include "windows_lean.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace svg_mb_control {

namespace {

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
        case RunMode::kEvidenceLog:
            return L"evidence-log";
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
        case RunMode::kEvidenceLog:
            return "evidence-log";
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

}  // namespace

bool IsLongRunningMode(RunMode mode) {
    return mode == RunMode::kReadLoop || mode == RunMode::kControlLoop;
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
    if (raw == L"evidence-log") {
        return RunMode::kEvidenceLog;
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
    if (value == "evidence-log") {
        return RunMode::kEvidenceLog;
    }
    throw std::runtime_error("Invalid default_mode in control config.");
}

int RequestStopAndWait(const std::filesystem::path& runtime_home,
                       bool quiet) {
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

    SupervisorState supervisor_state;
    supervisor_state.supervisor_pid = GetCurrentProcessId();
    WriteSupervisorState(runtime_home, supervisor_state);

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
        supervisor_state.last_worker_pid = worker.pid;
        supervisor_state.worker_restart_count = restart_count;
        supervisor_state.last_worker_started_time =
            FormatLocalIso8601(std::chrono::system_clock::now());
        WriteSupervisorState(runtime_home, supervisor_state);
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
        // On an intentional stop/restart a successor supervisor may already
        // have published its fresh control_supervisor.json; this exiting
        // supervisor must not clobber it with stale state. The JSONL event
        // below still records the exit for history. Crash/backoff restarts
        // (stop not requested) keep writing so repeated-crash visibility is
        // preserved.
        if (!stop_requested) {
            supervisor_state.has_last_worker_exit_code = true;
            supervisor_state.last_worker_exit_code =
                static_cast<std::int64_t>(exit_code);
            supervisor_state.last_worker_exit_time =
                FormatLocalIso8601(std::chrono::system_clock::now());
            WriteSupervisorState(runtime_home, supervisor_state);
        }
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
        supervisor_state.worker_restart_count = restart_count;
        supervisor_state.last_worker_restart_time =
            FormatLocalIso8601(std::chrono::system_clock::now());
        WriteSupervisorState(runtime_home, supervisor_state);
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

}  // namespace svg_mb_control
