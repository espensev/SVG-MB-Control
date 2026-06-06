#include "windows_lean.h"

#include <shellapi.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path CurrentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(buffer.data(), buffer.data() + length);
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

std::filesystem::path ResolveControlExe(
    const std::filesystem::path& runner_path) {
    const std::filesystem::path runner_dir = runner_path.parent_path();
    const std::array<std::filesystem::path, 2> candidates{
        runner_dir / "svg-mb-control.exe",
        runner_dir / "release" / "svg-mb-control.exe",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path ResolveDefaultConfig(
    const std::filesystem::path& control_exe) {
    if (control_exe.empty()) {
        return {};
    }
    return control_exe.parent_path() / "control.json";
}

std::wstring BuildControlCommandLine(
    const std::filesystem::path& control_exe,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& config_path) {
    std::wstring command_line = QuoteCommandLineArg(control_exe.wstring());
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += argument;
    }
    command_line += L" --config ";
    command_line += QuoteCommandLineArg(config_path.wstring());
    return command_line;
}

HANDLE OpenNul(DWORD desired_access) {
    return CreateFileW(L"NUL",
                       desired_access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

int RunHiddenAndWait(const std::filesystem::path& control_exe,
                     const std::vector<std::wstring>& arguments,
                     const std::filesystem::path& config_path) {
    HANDLE stdin_handle = OpenNul(GENERIC_READ);
    HANDLE stdout_handle = OpenNul(GENERIC_WRITE);
    HANDLE stderr_handle = OpenNul(GENERIC_WRITE);
    if (stdin_handle == INVALID_HANDLE_VALUE ||
        stdout_handle == INVALID_HANDLE_VALUE ||
        stderr_handle == INVALID_HANDLE_VALUE) {
        if (stdin_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_handle);
        }
        if (stdout_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(stdout_handle);
        }
        if (stderr_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(stderr_handle);
        }
        return 111;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_HIDE;
    startup_info.hStdInput = stdin_handle;
    startup_info.hStdOutput = stdout_handle;
    startup_info.hStdError = stderr_handle;

    PROCESS_INFORMATION process_info{};
    std::wstring command_line =
        BuildControlCommandLine(control_exe, arguments, config_path);
    const BOOL created = CreateProcessW(
        control_exe.wstring().c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        control_exe.parent_path().wstring().c_str(),
        &startup_info,
        &process_info);

    CloseHandle(stdin_handle);
    CloseHandle(stdout_handle);
    CloseHandle(stderr_handle);

    if (!created) {
        return 112;
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 1u;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

std::filesystem::path ParseConfigPath(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--config") {
            return std::filesystem::path(argv[i + 1]);
        }
    }
    return {};
}

bool HasFlag(int argc, wchar_t** argv, std::wstring_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == flag) {
            return true;
        }
    }
    return false;
}

int RunTaskRunner(int argc, wchar_t** argv) {
    const std::filesystem::path runner_path = CurrentExecutablePath();
    const std::filesystem::path control_exe = ResolveControlExe(runner_path);
    if (control_exe.empty()) {
        return 101;
    }

    std::filesystem::path config_path = ParseConfigPath(argc, argv);
    if (config_path.empty()) {
        config_path = ResolveDefaultConfig(control_exe);
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(config_path, ec)) {
        return 102;
    }

    if (HasFlag(argc, argv, L"--watchdog-run")) {
        const int health = RunHiddenAndWait(
            control_exe, {L"--health", L"--json"}, config_path);
        if (health == 0 || health == 1) {
            return 0;
        }
        if (health == 2) {
            return RunHiddenAndWait(control_exe, {L"--restart"}, config_path);
        }
        return health;
    }

    if (HasFlag(argc, argv, L"--start")) {
        return RunHiddenAndWait(control_exe, {L"--start"}, config_path);
    }

    if (HasFlag(argc, argv, L"--restart")) {
        return RunHiddenAndWait(control_exe, {L"--restart"}, config_path);
    }

    if (HasFlag(argc, argv, L"--stop")) {
        return RunHiddenAndWait(control_exe, {L"--stop"}, config_path);
    }

    if (HasFlag(argc, argv, L"--status")) {
        return RunHiddenAndWait(control_exe, {L"--status"}, config_path);
    }

    return 100;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 120;
    }
    const int result = RunTaskRunner(argc, argv);
    LocalFree(argv);
    return result;
}
