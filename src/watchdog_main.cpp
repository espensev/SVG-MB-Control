#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kHealthTimeoutMs = 30000u;
constexpr DWORD kRestartTimeoutMs = 90000u;

struct WatchdogArgs {
    std::filesystem::path exe_path;
    std::filesystem::path config_path;
    std::filesystem::path log_dir;
};

struct ChildResult {
    DWORD exit_code = 3u;
    bool started = false;
    bool timed_out = false;
};

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

std::wstring LastErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring text;
    if (length > 0 && buffer != nullptr) {
        text.assign(buffer, length);
        while (!text.empty() &&
               (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (text.empty()) {
        text = L"Windows error " + std::to_wstring(error);
    }
    return text;
}

std::string LocalTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_time);

    std::ostringstream stream;
    stream << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S");
    return stream.str();
}

void AppendLog(const std::filesystem::path& log_dir,
               std::string_view message) {
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    std::ofstream log(
        log_dir / "svg-mb-control-watchdog.log",
        std::ios::app | std::ios::binary);
    if (!log) {
        return;
    }
    log << '[' << LocalTimestamp() << "] " << message << '\n';
}

void AppendLog(const std::filesystem::path& log_dir,
               std::wstring_view message) {
    AppendLog(log_dir, WideToUtf8(message));
}

std::filesystem::path CurrentModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0u) {
            return {};
        }
        if (written < buffer.size() - 1u) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2u);
    }
}

std::wstring QuoteArg(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    std::wstring quoted;
    quoted.reserve(value.size() + 2u);
    quoted.push_back(L'"');
    for (wchar_t ch : value) {
        if (ch == L'"') {
            quoted.push_back(L'\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::filesystem::path& exe_path,
                              std::initializer_list<std::wstring> args) {
    std::wstring command_line = QuoteArg(exe_path);
    for (const auto& arg : args) {
        command_line.push_back(L' ');
        if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
            command_line += arg;
        } else {
            command_line.push_back(L'"');
            for (wchar_t ch : arg) {
                if (ch == L'"') {
                    command_line.push_back(L'\\');
                }
                command_line.push_back(ch);
            }
            command_line.push_back(L'"');
        }
    }
    return command_line;
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

ChildResult RunHiddenChild(const std::filesystem::path& exe_path,
                           const std::wstring& command_line,
                           const std::filesystem::path& working_directory,
                           const std::filesystem::path& stdout_path,
                           const std::filesystem::path& stderr_path,
                           DWORD timeout_ms,
                           const std::filesystem::path& log_dir) {
    std::error_code ec;
    std::filesystem::create_directories(stdout_path.parent_path(), ec);
    std::filesystem::create_directories(stderr_path.parent_path(), ec);

    HANDLE stdout_handle =
        OpenInheritedFile(stdout_path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (stdout_handle == INVALID_HANDLE_VALUE) {
        AppendLog(log_dir,
                  L"failed to open stdout log: " +
                      LastErrorText(GetLastError()));
        return {};
    }

    HANDLE stderr_handle =
        OpenInheritedFile(stderr_path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
        AppendLog(log_dir,
                  L"failed to open stderr log: " +
                      LastErrorText(GetLastError()));
        CloseHandle(stdout_handle);
        return {};
    }

    HANDLE stdin_handle = OpenInheritedFile("NUL", GENERIC_READ, OPEN_EXISTING);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        AppendLog(log_dir,
                  L"failed to open NUL stdin: " +
                      LastErrorText(GetLastError()));
        CloseHandle(stdout_handle);
        CloseHandle(stderr_handle);
        return {};
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_handle;
    startup_info.hStdOutput = stdout_handle;
    startup_info.hStdError = stderr_handle;

    PROCESS_INFORMATION process_info{};
    std::wstring mutable_command_line = command_line;
    const BOOL created = CreateProcessW(
        exe_path.wstring().c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        working_directory.wstring().c_str(),
        &startup_info,
        &process_info);

    CloseHandle(stdin_handle);
    CloseHandle(stdout_handle);
    CloseHandle(stderr_handle);

    if (!created) {
        AppendLog(log_dir,
                  L"failed to launch child: " + LastErrorText(GetLastError()));
        return {};
    }

    CloseHandle(process_info.hThread);

    ChildResult result;
    result.started = true;
    const DWORD wait_result = WaitForSingleObject(
        process_info.hProcess, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        result.exit_code = 3u;
        TerminateProcess(process_info.hProcess, result.exit_code);
        WaitForSingleObject(process_info.hProcess, 5000u);
    } else if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 3u;
        if (GetExitCodeProcess(process_info.hProcess, &exit_code)) {
            result.exit_code = exit_code;
        }
    } else {
        result.exit_code = 3u;
    }

    CloseHandle(process_info.hProcess);
    return result;
}

std::optional<std::vector<std::wstring>> CommandLineArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return std::nullopt;
    }

    std::vector<std::wstring> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    LocalFree(argv);
    return args;
}

std::optional<std::filesystem::path> RequirePathArg(
    const std::vector<std::wstring>& args,
    std::size_t* index,
    const std::filesystem::path& log_dir) {
    if (*index + 1u >= args.size()) {
        AppendLog(log_dir, L"missing value for " + args[*index]);
        return std::nullopt;
    }
    ++(*index);
    return std::filesystem::path(args[*index]);
}

std::optional<WatchdogArgs> ParseArgs(const std::filesystem::path& helper_path) {
    WatchdogArgs parsed;
    const std::filesystem::path helper_dir = helper_path.parent_path();
    parsed.exe_path = helper_dir / "svg-mb-control.exe";
    parsed.config_path = helper_dir / "control.json";
    parsed.log_dir = helper_dir / "runtime" / "logs";

    const auto maybe_args = CommandLineArgs();
    if (!maybe_args.has_value()) {
        AppendLog(parsed.log_dir, "failed to parse command line");
        return std::nullopt;
    }

    const auto& args = *maybe_args;
    for (std::size_t index = 1u; index < args.size(); ++index) {
        const std::wstring& arg = args[index];
        if (arg == L"--exe") {
            auto value = RequirePathArg(args, &index, parsed.log_dir);
            if (!value.has_value()) {
                return std::nullopt;
            }
            parsed.exe_path = *value;
        } else if (arg == L"--config") {
            auto value = RequirePathArg(args, &index, parsed.log_dir);
            if (!value.has_value()) {
                return std::nullopt;
            }
            parsed.config_path = *value;
        } else if (arg == L"--log-dir") {
            auto value = RequirePathArg(args, &index, parsed.log_dir);
            if (!value.has_value()) {
                return std::nullopt;
            }
            parsed.log_dir = *value;
        } else if (arg == L"--help" || arg == L"-h") {
            AppendLog(
                parsed.log_dir,
                "usage: svg-mb-control-watchdog.exe [--exe <path>] "
                "[--config <path>] [--log-dir <path>]");
            return std::nullopt;
        } else {
            AppendLog(parsed.log_dir, L"unknown argument: " + arg);
            return std::nullopt;
        }
    }

    parsed.exe_path =
        std::filesystem::absolute(parsed.exe_path).lexically_normal();
    parsed.config_path =
        std::filesystem::absolute(parsed.config_path).lexically_normal();
    parsed.log_dir =
        std::filesystem::absolute(parsed.log_dir).lexically_normal();
    return parsed;
}

int RunWatchdog() {
    const std::filesystem::path helper_path = CurrentModulePath();
    const auto maybe_args = ParseArgs(helper_path);
    if (!maybe_args.has_value()) {
        return 3;
    }
    const WatchdogArgs args = *maybe_args;

    std::error_code ec;
    std::filesystem::create_directories(args.log_dir, ec);
    AppendLog(args.log_dir,
              "watchdog run started exe=" + args.exe_path.string() +
                  " config=" + args.config_path.string());

    if (!std::filesystem::exists(args.exe_path, ec)) {
        AppendLog(args.log_dir, "control exe not found");
        return 3;
    }
    if (!std::filesystem::exists(args.config_path, ec)) {
        AppendLog(args.log_dir, "control config not found");
        return 3;
    }

    const std::filesystem::path health_stdout =
        args.log_dir / "svg-mb-control-watchdog.health.stdout.log";
    const std::filesystem::path health_stderr =
        args.log_dir / "svg-mb-control-watchdog.health.stderr.log";
    const std::wstring health_command = BuildCommandLine(
        args.exe_path,
        {L"--health", L"--json", L"--config", args.config_path.wstring()});
    const ChildResult health = RunHiddenChild(
        args.exe_path,
        health_command,
        args.exe_path.parent_path(),
        health_stdout,
        health_stderr,
        kHealthTimeoutMs,
        args.log_dir);

    if (!health.started) {
        AppendLog(args.log_dir, "health command failed to start");
        return 3;
    }
    if (health.timed_out) {
        AppendLog(args.log_dir, "health command timed out");
        return 3;
    }

    AppendLog(args.log_dir,
              "health exit code=" + std::to_string(health.exit_code));
    if (health.exit_code == 0u || health.exit_code == 1u) {
        AppendLog(args.log_dir, "watchdog run completed without restart");
        return 0;
    }
    if (health.exit_code != 2u) {
        AppendLog(args.log_dir, "failed health is not auto-restarted");
        return static_cast<int>(health.exit_code);
    }

    AppendLog(args.log_dir, "stale/stopped health; restarting control");
    const std::filesystem::path restart_stdout =
        args.log_dir / "svg-mb-control-watchdog.restart.stdout.log";
    const std::filesystem::path restart_stderr =
        args.log_dir / "svg-mb-control-watchdog.restart.stderr.log";
    const std::wstring restart_command = BuildCommandLine(
        args.exe_path,
        {L"--restart", L"--config", args.config_path.wstring()});
    const ChildResult restart = RunHiddenChild(
        args.exe_path,
        restart_command,
        args.exe_path.parent_path(),
        restart_stdout,
        restart_stderr,
        kRestartTimeoutMs,
        args.log_dir);

    if (!restart.started) {
        AppendLog(args.log_dir, "restart command failed to start");
        return 3;
    }
    if (restart.timed_out) {
        AppendLog(args.log_dir, "restart command timed out");
        return 3;
    }
    AppendLog(args.log_dir,
              "restart exit code=" + std::to_string(restart.exit_code));
    return static_cast<int>(restart.exit_code);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        return RunWatchdog();
    } catch (const std::exception& error) {
        const std::filesystem::path helper_path = CurrentModulePath();
        const std::filesystem::path log_dir =
            helper_path.parent_path() / "runtime" / "logs";
        AppendLog(log_dir,
                  std::string("unhandled watchdog exception: ") +
                      error.what());
    } catch (...) {
        const std::filesystem::path helper_path = CurrentModulePath();
        const std::filesystem::path log_dir =
            helper_path.parent_path() / "runtime" / "logs";
        AppendLog(log_dir, "unhandled watchdog exception");
    }
    return 3;
}
