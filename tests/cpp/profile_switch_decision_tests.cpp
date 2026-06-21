// Unit tests for the FEAT-0023 supervisor switch decision logic
// (DecideSwitchRequest / DecideAfterStartupOutcome), kept pure so the
// validate / last-known-good / auto-revert behavior is testable without
// spawning worker processes.

#include "profile_switch_decision.h"

#include "test_helpers.h"

#include <filesystem>
#include <optional>
#include <string>

namespace {

using svg_mb_control::DecideAfterStartupOutcome;
using svg_mb_control::DecideSwitchRequest;
using svg_mb_control::ProfileCatalogLookup;
using svg_mb_control::ProfileLoadValidator;
using svg_mb_control::ProfileSwitchState;
using svg_mb_control::RuntimeProfileSwitchRequest;
using svg_mb_control::StartupOutcomeAction;
using svg_mb_control::SwitchRequestAction;
using svg_mb_control::SwitchRequestDecision;

ProfileCatalogLookup Catalog() {
    return [](const std::string& name) -> std::filesystem::path {
        if (name == "quiet") return std::filesystem::path("/cat/quiet.json");
        return {};
    };
}

ProfileLoadValidator OkValidator() {
    return [](const std::filesystem::path&) -> std::optional<std::string> {
        return std::nullopt;
    };
}

ProfileLoadValidator ErrorValidator(const std::string& message) {
    return [message](
               const std::filesystem::path&) -> std::optional<std::string> {
        return message;
    };
}

RuntimeProfileSwitchRequest Req(const std::string& name,
                                const std::string& parse_error = "") {
    RuntimeProfileSwitchRequest request;
    request.profile_name = name;
    request.parse_error = parse_error;
    return request;
}

bool IsActivate(const SwitchRequestDecision& decision) {
    return decision.action == SwitchRequestAction::kActivate;
}

void TestValidRequestActivates() {
    const auto d = DecideSwitchRequest(Req("quiet"), Catalog(), OkValidator());
    ExpectTrue(IsActivate(d), "a valid switch request activates");
    ExpectEqual(d.config_path.generic_string(), "/cat/quiet.json",
                "activate carries the resolved config path");
    ExpectEqual(d.profile_name, "quiet", "activate carries the profile name");
}

void TestParseErrorRejects() {
    const auto d =
        DecideSwitchRequest(Req("quiet", "bad json"), Catalog(), OkValidator());
    ExpectFalse(IsActivate(d), "a request with a parse_error is rejected");
}

void TestEmptyNameRejects() {
    const auto d = DecideSwitchRequest(Req(""), Catalog(), OkValidator());
    ExpectFalse(IsActivate(d), "an empty profile name is rejected");
}

void TestUnknownProfileRejects() {
    const auto d = DecideSwitchRequest(Req("nope"), Catalog(), OkValidator());
    ExpectFalse(IsActivate(d), "a profile not in the catalog is rejected");
}

void TestValidatorErrorRejects() {
    const auto d = DecideSwitchRequest(Req("quiet"), Catalog(),
                                       ErrorValidator("load failed"));
    ExpectFalse(IsActivate(d), "a candidate that fails validation is rejected");
    ExpectTrue(d.detail.find("load failed") != std::string::npos,
               "the rejection detail carries the validator error");
}

ProfileSwitchState State() {
    ProfileSwitchState s;
    s.active_profile_name = "quiet";
    s.active_config_path = "/cat/quiet.json";
    s.last_known_good_profile_name = "default";
    s.last_known_good_config_path = "/cat/default.json";
    return s;
}

void TestSurvivalPromotesLastKnownGood() {
    auto s = State();
    const auto d = DecideAfterStartupOutcome(s, true);
    ExpectTrue(d.action == StartupOutcomeAction::kKeep,
               "a worker surviving startup keeps its profile");
    ExpectEqual(s.last_known_good_profile_name, "quiet",
                "survival promotes the active profile to last-known-good (name)");
    ExpectEqual(s.last_known_good_config_path.generic_string(),
                "/cat/quiet.json",
                "survival promotes the active config to last-known-good (path)");
}

void TestStartupFailureReverts() {
    auto s = State();
    const auto d = DecideAfterStartupOutcome(s, false);
    ExpectTrue(d.action == StartupOutcomeAction::kRevertToLastKnownGood,
               "a failed startup reverts to last-known-good");
    ExpectEqual(s.active_profile_name, "default",
                "revert restores the active profile to last-known-good (name)");
    ExpectEqual(s.active_config_path.generic_string(), "/cat/default.json",
                "revert restores the active config to last-known-good (path)");
}

}  // namespace

int main() {
    TestValidRequestActivates();
    TestParseErrorRejects();
    TestEmptyNameRejects();
    TestUnknownProfileRejects();
    TestValidatorErrorRejects();
    TestSurvivalPromotesLastKnownGood();
    TestStartupFailureReverts();
    if (g_failures == 0) {
        std::cout << "profile_switch_decision_tests: all passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
