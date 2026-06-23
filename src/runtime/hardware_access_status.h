#pragma once

#include <string>
#include <string_view>

namespace svg_mb_control {

enum class HardwareAccessState {
    kUnknown,
    kAvailable,
    kUnavailable,
};

struct HardwareAccessStatus {
    HardwareAccessState read_state = HardwareAccessState::kUnknown;
    HardwareAccessState write_state = HardwareAccessState::kUnknown;
    std::string read_detail;
    std::string write_detail;
};

enum class HardwareAccessTransition {
    kNone,
    kUnavailable,
    kRestored,
};

const char* HardwareAccessStateName(HardwareAccessState state);
HardwareAccessState ParseHardwareAccessState(std::string_view state);
HardwareAccessState HardwareAccessOverallState(
    const HardwareAccessStatus& status);
bool HardwareAccessUnavailable(const HardwareAccessStatus& status);
HardwareAccessTransition ClassifyHardwareAccessTransition(
    const HardwareAccessStatus& previous,
    const HardwareAccessStatus& current);
std::string FormatHardwareAccessDetail(const HardwareAccessStatus& status);

}  // namespace svg_mb_control
