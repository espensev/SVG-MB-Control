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
#include "runtime_singleton.h"
#include "runtime_status.h"
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
    // CreateFileW with FILE_SHARE_DELETE so we never block the inherited
    // child stderr handle that the supervisor opened with the same share
    // flags. The supervisor only calls this after WaitForSingleObject on
    // the worker, but uniform share semantics avoid surprises if that
    // ordering ever changes.
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0) {
        CloseHandle(handle);
        return {};
    }

    const auto length = static_cast<std::uintmax_t>(size.QuadPart);
    const auto start = length > max_bytes ? length - max_bytes : 0u;
    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(start);
    if (!SetFilePointerEx(handle, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(handle);
        return {};
    }

    std::string content;
    content.reserve(static_cast<std::size_t>(length - start));
    constexpr DWORD kChunk = 64u * 1024u;
    std::vector<char> buffer(kChunk);
    for (;;) {
        DWORD bytes_read = 0u;
        if (!ReadFile(handle, buffer.data(), kChunk, &bytes_read, nullptr) ||
            bytes_read == 0u) {
            break;
        }
        content.append(buffer.data(), bytes_read);
    }
    CloseHandle(handle);
    return content;
}

bool SupervisorSingletonHeld(const std::filesystem::path& runtime_home) {
    return ProbeRuntimeSingletonHeld(SingletonRole::kSupervisor, runtime_home);
}

bool PrintAlreadyRunningIfActive(RunMode mode,
                                 const std::filesystem::path& runtime_home) {
    // Supervisor mutex is the authoritative signal: it is held continuously
    // from supervisor start through any worker restart/backoff window, so the
    // launcher can detect duplicates even when control_runtime.json is
    // momentarily stale (worker crashed and supervisor is about to respawn).
    const bool supervisor_held = SupervisorSingletonHeld(runtime_home);
    if (supervisor_held) {
        const auto supervisor_state = ReadSupervisorState(runtime_home);
        const std::uint32_t supervisor_pid =
            supervisor_state.has_value() ? supervisor_state->supervisor_pid
                                         : 0u;
        const auto worker_status = ReadRuntimeStatus(runtime_home);
        const std::uint32_t worker_pid =
            worker_status.has_value() ? worker_status->process_id : 0u;
        const std::string active_mode =
            (worker_status.has_value() && !worker_status->mode.empty())
                ? worker_status->mode
                : std::string(RunModeLabel(mode));
        std::cout << "svg-mb-control: " << active_mode
                  << " is already running\n"
                  << "  supervisor_pid: " << supervisor_pid << '\n'
                  << "  worker_pid: " << worker_pid << '\n'
                  << "  requested_mode: " << RunModeLabel(mode) << '\n'
                  << "  status: " << RuntimeStatusPath(runtime_home).string()
                  << '\n'
                  << "  runtime_home: " << runtime_home.string() << '\n';
        return true;
    }

    // Fallback for the rare case where a worker is somehow running without a
    // supervisor (e.g., direct --run-foreground invocation in a dev workflow).
    const auto status = ReadRuntimeStatus(runtime_home);
    if (!status.has_value() || !status->LooksActive()) {
        return false;
    }

    const std::string active_mode =
        status->mode.empty() ? std::string(RunModeLabel(mode)) : status->mode;
    std::cout << "svg-mb-control: " << active_mode
              << " is already running\n"
              << "  pid: " << status->process_id << '\n'
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
        const auto status = ReadRuntimeStatus(runtime_home);
        const bool worker_stopped =
            !status.has_value() || !status->LooksActive();
        if (worker_stopped && !SupervisorSingletonHeld(runtime_home)) {
            return true;
        }
        if (status.has_value() && status->status == "shutdown" &&
            !SupervisorSingletonHeld(runtime_home)) {
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

        // Exit code 2 from the supervisor means singleton acquisition
        // refused (see RunSupervisedLongRunningMode). The race window:
        // PrintAlreadyRunningIfActive above returned false, then a
        // competing launcher acquired the supervisor mutex before our
        // child reached CreateMutexW. Re-probe and surface the friendly
        // already-running message instead of dumping the child's stderr.
        if (exit_code == 2u &&
            PrintAlreadyRunningIfActive(mode, runtime_home)) {
            return 0;
        }

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
              << "  status: " << RuntimeStatusPath(runtime_home).string()
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

namespace {

// Restart-backoff loop body for RunSupervisedLongRunningMode. Spawns the
// worker, waits, records exit state, applies backoff, and repeats until
// either a stop is requested or the worker exits cleanly. Returns 0 on
// graceful exit; non-zero is the startup-failure exit code that the parent
// surfaces to the caller (the shutdown event fires only on a 0 return).
int RunSupervisorWorkerLoop(
    RunMode mode,
    const std::filesystem::path& exe_path,
    const std::filesystem::path& config_source_path,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& runtime_home,
    const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path,
    SupervisorState& supervisor_state) {
    std::uint32_t restart_count = 0u;

    while (!svg_mb_control::RuntimeStopRequested(runtime_home)) {
        StartedProcess worker = StartHiddenProcess(
            exe_path,
            BuildManagedCommandLine(exe_path, mode, config_source_path, false),
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
        // The supervisor singleton guarantees no successor supervisor can
        // be writing control_supervisor.json concurrently, so it is safe to
        // record the exit unconditionally; this preserves the last worker
        // outcome even for graceful stops.
        supervisor_state.has_last_worker_exit_code = true;
        supervisor_state.last_worker_exit_code =
            static_cast<std::int64_t>(exit_code);
        supervisor_state.last_worker_exit_time =
            FormatLocalIso8601(std::chrono::system_clock::now());
        WriteSupervisorState(runtime_home, supervisor_state);
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
    return 0;
}

}  // namespace

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

    // Singleton: only one supervisor per runtime_home. Acquire before any
    // sidecar write or stop-request mutation so a duplicate launch can never
    // clobber the live supervisor's state.
    SingletonAcquisition supervisor_singleton =
        TryAcquireRuntimeSingleton(SingletonRole::kSupervisor, runtime_home);
    if (!supervisor_singleton.acquired) {
        const auto existing = ReadSupervisorState(runtime_home);
        std::cerr << "Error: another svg-mb-control supervisor is already "
                  << "running for runtime_home=" << runtime_home.string()
                  << '\n';
        if (existing.has_value() && existing->supervisor_pid != 0u) {
            std::cerr << "  active_supervisor_pid: "
                      << existing->supervisor_pid << '\n';
        }
        if (!supervisor_singleton.diagnostic.empty()) {
            std::cerr << "  detail: " << supervisor_singleton.diagnostic
                      << '\n';
        }
        return 2;
    }

    svg_mb_control::ClearRuntimeStopRequest(runtime_home);

    const std::filesystem::path working_directory = exe_path.parent_path();
    const std::filesystem::path stdout_path =
        runtime_home / "svg-mb-control.worker.stdout.log";
    const std::filesystem::path stderr_path =
        runtime_home / "svg-mb-control.worker.stderr.log";

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

    const int loop_exit = RunSupervisorWorkerLoop(
        mode, exe_path, config.source_path, working_directory, runtime_home,
        stdout_path, stderr_path, supervisor_state);
    if (loop_exit != 0) {
        return loop_exit;
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
