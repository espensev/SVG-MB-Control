#include "bench_bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

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

namespace {

constexpr std::size_t kTailBufferMaxBytes = 64u * 1024u;

void AppendBounded(std::string& buffer, const char* data, std::size_t size) {
    buffer.append(data, data + size);
    if (buffer.size() > kTailBufferMaxBytes) {
        buffer.erase(0, buffer.size() - kTailBufferMaxBytes);
    }
}

}  // namespace

struct BenchChildSupervisor::Impl {
    std::wstring bench_exe_path;
    std::vector<std::wstring> args;

    HANDLE process_handle = nullptr;
    DWORD process_id = 0u;

    std::thread stdout_thread;
    std::thread stderr_thread;

    mutable std::mutex state_mutex;
    std::string stdout_tail;
    std::string stderr_tail;
    std::atomic<bool> started{false};
    std::atomic<bool> exit_observed{false};
    std::atomic<int> last_exit_code{-1};

    ~Impl() {
        if (stdout_thread.joinable()) {
            stdout_thread.join();
        }
        if (stderr_thread.joinable()) {
            stderr_thread.join();
        }
        if (process_handle != nullptr) {
            CloseHandle(process_handle);
            process_handle = nullptr;
        }
    }
};

namespace {

void DrainPipe(HANDLE pipe_handle,
               std::mutex* mutex,
               std::string* tail) {
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
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
                break;
            }
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(*mutex);
        AppendBounded(*tail, buffer.data(), static_cast<std::size_t>(bytes_read));
    }
    CloseHandle(pipe_handle);
}

}  // namespace

BenchChildSupervisor::BenchChildSupervisor(std::wstring bench_exe_path,
                                           std::vector<std::wstring> args)
    : impl_(std::make_unique<Impl>()) {
    impl_->bench_exe_path = std::move(bench_exe_path);
    impl_->args = std::move(args);
}

BenchChildSupervisor::~BenchChildSupervisor() {
    if (impl_ != nullptr) {
        RequestStop(2000u);
    }
}

void BenchChildSupervisor::Start() {
    if (impl_->started.load()) {
        throw std::runtime_error("BenchChildSupervisor already started.");
    }
    if (impl_->bench_exe_path.empty()) {
        throw std::runtime_error("Bench executable path must not be empty.");
    }

    const std::filesystem::path exe_path = std::filesystem::absolute(
        std::filesystem::path(impl_->bench_exe_path)).lexically_normal();
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
    std::wstring command_line = BuildCommandLine(exe_path.wstring(), impl_->args);
    std::wstring working_directory = exe_path.parent_path().wstring();

    const DWORD creation_flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(exe_path.c_str(),
                        command_line.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        creation_flags,
                        nullptr,
                        working_directory.empty() ? nullptr : working_directory.c_str(),
                        &startup_info,
                        &process_info)) {
        throw std::runtime_error("CreateProcessW failed: " +
                                 Win32Message(GetLastError()));
    }

    CloseHandle(process_info.hThread);

    impl_->process_handle = process_info.hProcess;
    impl_->process_id = process_info.dwProcessId;

    stdout_write.reset();
    stderr_write.reset();

    const HANDLE stdout_read_raw = stdout_read.release();
    const HANDLE stderr_read_raw = stderr_read.release();
    impl_->stdout_thread = std::thread(DrainPipe,
                                       stdout_read_raw,
                                       &impl_->state_mutex,
                                       &impl_->stdout_tail);
    impl_->stderr_thread = std::thread(DrainPipe,
                                       stderr_read_raw,
                                       &impl_->state_mutex,
                                       &impl_->stderr_tail);

    impl_->started.store(true);
}

bool BenchChildSupervisor::IsRunning() {
    if (!impl_->started.load() || impl_->exit_observed.load()) {
        return false;
    }
    if (impl_->process_handle == nullptr) {
        return false;
    }

    DWORD exit_code = 0u;
    if (!GetExitCodeProcess(impl_->process_handle, &exit_code)) {
        return false;
    }
    if (exit_code == STILL_ACTIVE) {
        return true;
    }

    impl_->last_exit_code.store(static_cast<int>(exit_code));
    impl_->exit_observed.store(true);
    return false;
}

int BenchChildSupervisor::LastExitCode() {
    // Refresh state.
    (void)IsRunning();
    return impl_->last_exit_code.load();
}

std::string BenchChildSupervisor::StdoutTail() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->stdout_tail;
}

std::string BenchChildSupervisor::StderrTail() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->stderr_tail;
}

void BenchChildSupervisor::RequestStop(std::uint32_t graceful_timeout_ms) {
    if (!impl_->started.load() || impl_->exit_observed.load()) {
        if (impl_->stdout_thread.joinable()) {
            impl_->stdout_thread.join();
        }
        if (impl_->stderr_thread.joinable()) {
            impl_->stderr_thread.join();
        }
        return;
    }
    if (impl_->process_handle == nullptr) {
        return;
    }

    // Send CTRL_BREAK_EVENT to the child's process group. The child was
    // launched with CREATE_NEW_PROCESS_GROUP, so process_id is the group id.
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, impl_->process_id);

    const DWORD wait_result = WaitForSingleObject(impl_->process_handle,
                                                  graceful_timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(impl_->process_handle, 124);
        WaitForSingleObject(impl_->process_handle, INFINITE);
    }

    DWORD exit_code = 0u;
    if (GetExitCodeProcess(impl_->process_handle, &exit_code)) {
        impl_->last_exit_code.store(static_cast<int>(exit_code));
    }
    impl_->exit_observed.store(true);

    if (impl_->stdout_thread.joinable()) {
        impl_->stdout_thread.join();
    }
    if (impl_->stderr_thread.joinable()) {
        impl_->stderr_thread.join();
    }
}

}  // namespace svg_mb_control
