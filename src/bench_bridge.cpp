#include "bench_bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace svg_mb_control {

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~UniqueHandle() {
        reset();
    }

    HANDLE get() const noexcept {
        return handle_;
    }

    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::filesystem::path CurrentExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("Could not resolve current executable path.");
    }
    return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string output(static_cast<std::size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()),
                                            output.data(), size, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    return output;
}

std::string Win32Message(DWORD error_code) {
    LPWSTR message_buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags,
                                        nullptr,
                                        error_code,
                                        0,
                                        reinterpret_cast<LPWSTR>(&message_buffer),
                                        0,
                                        nullptr);
    std::wstring message;
    if (length > 0 && message_buffer != nullptr) {
        message.assign(message_buffer, message_buffer + length);
        LocalFree(message_buffer);
    }

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || std::iswspace(message.back()) != 0)) {
        message.pop_back();
    }

    if (message.empty()) {
        return "Win32 error " + std::to_string(error_code);
    }
    return Narrow(message);
}

std::wstring QuoteWindowsArg(std::wstring_view arg) {
    if (arg.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = false;
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::wstring(arg);
    }

    std::wstring quoted;
    quoted.push_back(L'"');

    std::size_t backslash_count = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslash_count;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        quoted.append(backslash_count, L'\\');
        backslash_count = 0;
        quoted.push_back(ch);
    }

    quoted.append(backslash_count * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::wstring& exe_path,
                              const std::vector<std::wstring>& args) {
    std::wstring command_line = QuoteWindowsArg(exe_path);
    for (const auto& arg : args) {
        command_line.push_back(L' ');
        command_line += QuoteWindowsArg(arg);
    }
    return command_line;
}

std::string ReadAllFromPipe(HANDLE pipe_handle) {
    std::string output;
    std::array<char, 4096> buffer{};

    for (;;) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(pipe_handle,
                                 buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytes_read,
                                 nullptr);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                break;
            }
            throw std::runtime_error("ReadFile failed while capturing child output: " +
                                     Win32Message(error));
        }
        if (bytes_read == 0) {
            break;
        }
        output.append(buffer.data(), buffer.data() + bytes_read);
    }

    return output;
}

std::string Trim(std::string value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string ExtractSummaryValue(const std::string& stdout_text,
                                const std::string& key) {
    std::istringstream stream(stdout_text);
    std::string line;
    const std::string prefix = key + ":";
    while (std::getline(stream, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return Trim(line.substr(prefix.size()));
        }
    }
    return {};
}

bool LooksLikeJsonObject(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    if (start >= text.size() || text[start] != '{') {
        return false;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return end > start && text[end - 1] == '}';
}

JsonArtifactLaunchResult RunBenchJsonArtifactCommand(
    const std::wstring& bench_exe_path,
    const std::vector<std::wstring>& args,
    std::uint32_t timeout_ms,
    std::string_view command_name,
    std::string_view summary_key) {
    JsonArtifactLaunchResult result;
    result.process = RunBenchProcess(bench_exe_path, args, timeout_ms);

    if (result.process.exit_code != 0) {
        throw std::runtime_error("Bench " + std::string(command_name) +
                                 " exited with code " +
                                 std::to_string(result.process.exit_code) + ".");
    }

    const std::string artifact_path = ExtractSummaryValue(
        result.process.stdout_text, std::string(summary_key));
    if (artifact_path.empty()) {
        throw std::runtime_error("Bench stdout did not include " +
                                 std::string(summary_key) + ".");
    }

    result.json_artifact_path = std::filesystem::absolute(
        std::filesystem::path(artifact_path)).lexically_normal();
    return result;
}

}  // namespace

std::filesystem::path ResolveDefaultBenchExecutablePath() {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path exe_dir = CurrentExecutableDirectory();
    const std::filesystem::path cwd = std::filesystem::current_path();

    candidates.push_back(exe_dir / "svg-mb-bench.exe");
    candidates.push_back(exe_dir.parent_path() / "svg-mb-bench.exe");
    candidates.push_back(exe_dir.parent_path().parent_path().parent_path() /
                         "SVG-MB-Bench" / "svg-mb-bench.exe");
    candidates.push_back(cwd / "svg-mb-bench.exe");
    candidates.push_back(cwd / "SVG-MB-Bench" / "svg-mb-bench.exe");
    candidates.push_back(cwd.parent_path() / "SVG-MB-Bench" / "svg-mb-bench.exe");

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    return {};
}

BridgeProcessResult RunBenchProcess(const std::wstring& bench_exe_path,
                                    const std::vector<std::wstring>& args,
                                    std::uint32_t timeout_ms) {
    if (bench_exe_path.empty()) {
        throw std::runtime_error("Bench executable path must not be empty.");
    }

    const std::filesystem::path exe_path = std::filesystem::absolute(
        std::filesystem::path(bench_exe_path)).lexically_normal();
    if (!std::filesystem::exists(exe_path)) {
        throw std::runtime_error("Bench executable not found: " + exe_path.string());
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE raw_stdout_read = nullptr;
    HANDLE raw_stdout_write = nullptr;
    HANDLE raw_stderr_read = nullptr;
    HANDLE raw_stderr_write = nullptr;

    if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0)) {
        throw std::runtime_error("CreatePipe failed for stdout: " +
                                 Win32Message(GetLastError()));
    }
    UniqueHandle stdout_read(raw_stdout_read);
    UniqueHandle stdout_write(raw_stdout_write);

    if (!CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0)) {
        throw std::runtime_error("CreatePipe failed for stderr: " +
                                 Win32Message(GetLastError()));
    }
    UniqueHandle stderr_read(raw_stderr_read);
    UniqueHandle stderr_write(raw_stderr_write);

    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation failed for stdout read handle: " +
                                 Win32Message(GetLastError()));
    }
    if (!SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation failed for stderr read handle: " +
                                 Win32Message(GetLastError()));
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = stdout_write.get();
    startup_info.hStdError = stderr_write.get();

    PROCESS_INFORMATION process_info{};
    std::wstring command_line = BuildCommandLine(exe_path.wstring(), args);
    std::wstring working_directory = exe_path.parent_path().wstring();

    if (!CreateProcessW(exe_path.c_str(),
                        command_line.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        working_directory.empty() ? nullptr : working_directory.c_str(),
                        &startup_info,
                        &process_info)) {
        throw std::runtime_error("CreateProcessW failed: " +
                                 Win32Message(GetLastError()));
    }

    UniqueHandle process_handle(process_info.hProcess);
    [[maybe_unused]] UniqueHandle thread_handle(process_info.hThread);

    stdout_write.reset();
    stderr_write.reset();

    const DWORD wait_result = WaitForSingleObject(process_handle.get(), timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process_handle.get(), 124);
        WaitForSingleObject(process_handle.get(), INFINITE);
        throw std::runtime_error("Bench process timed out after " +
                                 std::to_string(timeout_ms) + " ms.");
    }
    if (wait_result != WAIT_OBJECT_0) {
        throw std::runtime_error("WaitForSingleObject failed for Bench process: " +
                                 Win32Message(GetLastError()));
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed: " +
                                 Win32Message(GetLastError()));
    }

    BridgeProcessResult result;
    result.exit_code = static_cast<int>(exit_code);
    result.stdout_text = ReadAllFromPipe(stdout_read.get());
    result.stderr_text = ReadAllFromPipe(stderr_read.get());
    return result;
}

JsonArtifactLaunchResult RunReadSnapshot(const std::wstring& bench_exe_path,
                                         std::uint32_t timeout_ms) {
    return RunBenchJsonArtifactCommand(
        bench_exe_path,
        {L"read-snapshot"},
        timeout_ms,
        "read-snapshot",
        "snapshot_archive");
}

JsonArtifactLaunchResult RunLoggerService(const std::wstring& bench_exe_path,
                                          std::uint32_t duration_ms,
                                          std::uint32_t timeout_ms) {
    if (duration_ms == 0u) {
        throw std::runtime_error("logger-service duration_ms must be greater than zero.");
    }

    return RunBenchJsonArtifactCommand(
        bench_exe_path,
        {L"logger-service", L"--duration-ms", std::to_wstring(duration_ms)},
        timeout_ms,
        "logger-service",
        "snapshot_path");
}

std::string LoadJsonObjectFile(const std::filesystem::path& json_path) {
    if (json_path.empty()) {
        throw std::runtime_error("JSON path must not be empty.");
    }
    if (!std::filesystem::exists(json_path)) {
        throw std::runtime_error("JSON file not found: " +
                                 json_path.string());
    }

    std::ifstream stream(json_path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Could not open JSON file: " +
                                 json_path.string());
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string json_text = buffer.str();
    if (json_text.empty()) {
        throw std::runtime_error("JSON file is empty: " +
                                 json_path.string());
    }
    if (!LooksLikeJsonObject(json_text)) {
        throw std::runtime_error("JSON file is not a JSON object: " +
                                 json_path.string());
    }

    return json_text;
}

}  // namespace svg_mb_control
