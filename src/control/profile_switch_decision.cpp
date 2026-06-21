#include "profile_switch_decision.h"

namespace svg_mb_control {

SwitchRequestDecision DecideSwitchRequest(
    const RuntimeProfileSwitchRequest& request,
    const ProfileCatalogLookup& lookup,
    const ProfileLoadValidator& validator) {
    SwitchRequestDecision decision;
    decision.profile_name = request.profile_name;

    if (!request.parse_error.empty()) {
        decision.detail = "malformed switch request: " + request.parse_error;
        return decision;  // kReject (default)
    }
    if (request.profile_name.empty()) {
        decision.detail = "switch request names no profile";
        return decision;
    }
    const std::filesystem::path candidate = lookup(request.profile_name);
    if (candidate.empty()) {
        decision.detail =
            "profile not found in catalog: " + request.profile_name;
        return decision;
    }
    if (const std::optional<std::string> error = validator(candidate)) {
        decision.detail = "profile failed validation: " + *error;
        return decision;
    }

    decision.action = SwitchRequestAction::kActivate;
    decision.config_path = candidate;
    decision.detail = "switching to profile " + request.profile_name;
    return decision;
}

StartupOutcomeDecision DecideAfterStartupOutcome(
    ProfileSwitchState& state, bool worker_survived_startup) {
    StartupOutcomeDecision decision;
    if (worker_survived_startup) {
        // The active profile produced a worker that cleared startup; promote it
        // to last-known-good.
        state.last_known_good_profile_name = state.active_profile_name;
        state.last_known_good_config_path = state.active_config_path;
        decision.action = StartupOutcomeAction::kKeep;
    } else {
        // A (switched-in) profile failed startup; revert to last-known-good.
        state.active_profile_name = state.last_known_good_profile_name;
        state.active_config_path = state.last_known_good_config_path;
        decision.action = StartupOutcomeAction::kRevertToLastKnownGood;
    }
    decision.config_path = state.active_config_path;
    decision.profile_name = state.active_profile_name;
    return decision;
}

}  // namespace svg_mb_control
