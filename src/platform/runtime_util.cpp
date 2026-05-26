#include "runtime_util.h"

#include "windows_lean.h"

#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace svg_mb_control {

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    return written > 0u ? std::string(buffer.data(), written) : std::string();
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

std::optional<std::chrono::system_clock::time_point> ParseLocalIso8601(
    std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::tm tm_value{};
    std::istringstream stream{std::string(value)};
    stream >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm_value.tm_isdst = -1;
    const std::time_t tt = std::mktime(&tm_value);
    if (tt == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace svg_mb_control
