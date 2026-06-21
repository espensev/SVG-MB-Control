#pragma once

// FEAT-0023 supervisor switch DECISION logic, kept pure so it is unit-testable
// without spawning worker processes. The supervisor loop is the impure shell
// (spawn / signal / wait); these functions decide what it should do.

#include "machine_profile.h"     // ProfileCatalogLookup
#include "runtime_lifecycle.h"   // RuntimeProfileSwitchRequest

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace svg_mb_control {

// Returns std::nullopt when the candidate config at the path loads/validates,
// or an error message otherwise. Production wraps LoadControlConfig — a
// parse-only check; the hardware bind happens later in the spawned worker (the
// known D-MPROFILE-5 limit that auto-revert backstops).
using ProfileLoadValidator =
    std::function<std::optional<std::string>(const std::filesystem::path&)>;

struct ProfileSwitchState {
    std::string active_profile_name;
    std::filesystem::path active_config_path;
    std::string last_known_good_profile_name;
    std::filesystem::path last_known_good_config_path;
};

enum class SwitchRequestAction { kReject, kActivate };

struct SwitchRequestDecision {
    SwitchRequestAction action = SwitchRequestAction::kReject;
    std::filesystem::path config_path;  // resolved candidate (kActivate only)
    std::string profile_name;
    std::string detail;  // rejection reason, or the applied profile detail
};

// REQ-MPROFILE-05: decide whether a taken switch request should activate. Reject
// (with a detail reason, leaving the running worker untouched) when the request
// carries a parse_error, names an empty or unknown-to-the-catalog profile, or
// the candidate config fails to load/validate. Otherwise activate, carrying the
// resolved config path and profile name. Pure — does not mutate any state.
SwitchRequestDecision DecideSwitchRequest(
    const RuntimeProfileSwitchRequest& request,
    const ProfileCatalogLookup& lookup,
    const ProfileLoadValidator& validator);

enum class StartupOutcomeAction { kKeep, kRevertToLastKnownGood };

struct StartupOutcomeDecision {
    StartupOutcomeAction action = StartupOutcomeAction::kKeep;
    std::filesystem::path config_path;  // the active config after the decision
    std::string profile_name;
};

// REQ-MPROFILE-07: fold in a (possibly just-switched) worker's startup outcome.
// On survival: promote the active profile to last-known-good and keep it. On a
// startup failure: revert the active profile to last-known-good. Mutates state
// accordingly and returns the resulting active config/profile.
StartupOutcomeDecision DecideAfterStartupOutcome(ProfileSwitchState& state,
                                                 bool worker_survived_startup);

}  // namespace svg_mb_control
