#include "hardware_access_status.h"

#include <sstream>

namespace svg_mb_control {

const char* HardwareAccessStateName(HardwareAccessState state) {
    switch (state) {
        case HardwareAccessState::kUnknown:
            return "unknown";
        case HardwareAccessState::kAvailable:
            return "available";
        case HardwareAccessState::kUnavailable:
            return "unavailable";
    }
    return "unknown";
}

HardwareAccessState ParseHardwareAccessState(std::string_view state) {
    if (state == "available") {
        return HardwareAccessState::kAvailable;
    }
    if (state == "unavailable") {
        return HardwareAccessState::kUnavailable;
    }
    return HardwareAccessState::kUnknown;
}

HardwareAccessState HardwareAccessOverallState(
    const HardwareAccessStatus& status) {
    if (status.read_state == HardwareAccessState::kUnavailable ||
        status.write_state == HardwareAccessState::kUnavailable) {
        return HardwareAccessState::kUnavailable;
    }
    if (status.read_state == HardwareAccessState::kAvailable &&
        status.write_state == HardwareAccessState::kAvailable) {
        return HardwareAccessState::kAvailable;
    }
    return HardwareAccessState::kUnknown;
}

bool HardwareAccessUnavailable(const HardwareAccessStatus& status) {
    return HardwareAccessOverallState(status) ==
           HardwareAccessState::kUnavailable;
}

HardwareAccessTransition ClassifyHardwareAccessTransition(
    const HardwareAccessStatus& previous,
    const HardwareAccessStatus& current) {
    const HardwareAccessState previous_overall =
        HardwareAccessOverallState(previous);
    const HardwareAccessState current_overall =
        HardwareAccessOverallState(current);
    if (current_overall == HardwareAccessState::kUnavailable &&
        previous_overall != HardwareAccessState::kUnavailable) {
        return HardwareAccessTransition::kUnavailable;
    }
    if (previous_overall == HardwareAccessState::kUnavailable &&
        current_overall == HardwareAccessState::kAvailable) {
        return HardwareAccessTransition::kRestored;
    }
    return HardwareAccessTransition::kNone;
}

std::string FormatHardwareAccessDetail(const HardwareAccessStatus& status) {
    std::ostringstream detail;
    detail << "hardware access state="
           << HardwareAccessStateName(HardwareAccessOverallState(status))
           << " read=" << HardwareAccessStateName(status.read_state)
           << " write=" << HardwareAccessStateName(status.write_state);
    if (!status.read_detail.empty()) {
        detail << " read_detail=" << status.read_detail;
    }
    if (!status.write_detail.empty()) {
        detail << " write_detail=" << status.write_detail;
    }
    return detail.str();
}

}  // namespace svg_mb_control
