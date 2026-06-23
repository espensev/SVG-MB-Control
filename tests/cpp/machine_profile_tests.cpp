// Unit tests for FEAT-0023 profile-selection precedence
// (ResolveProfileSelection). The catalog lookup is injected, so these tests
// exercise the precedence rules without a filesystem or host-name dependency.
//
// Precedence, highest first:
//   explicit --config > --profile flag > SVG_MB_PROFILE env > machine id > default.
// Explicit selectors (flag/env) error on an unknown name; machine id is
// best-effort and falls through to the default when unknown.

#include "machine_profile.h"

#include "test_helpers.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using svg_mb_control::ProfileCatalogLookup;
using svg_mb_control::ProfileResolution;
using svg_mb_control::ProfileResolutionRequest;
using svg_mb_control::ProfileResolutionSourceLabel;
using svg_mb_control::ResolveProfileSelection;

void ExpectThrowsContaining(const std::function<void()>& thunk,
                            std::string_view needle,
                            const std::string& message) {
    try {
        thunk();
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected throw containing ["
                  << needle << "], but no exception\n";
    } catch (const std::exception& ex) {
        const std::string what(ex.what());
        if (what.find(needle) == std::string::npos) {
            ++g_failures;
            std::cerr << "FAIL: " << message << " expected throw containing ["
                      << needle << "], got [" << what << "]\n";
        }
    }
}

std::string SourceLabel(const ProfileResolution& res) {
    return std::string(ProfileResolutionSourceLabel(res.source));
}

// A catalog with two named profiles; unknown names resolve to an empty path.
ProfileCatalogLookup TwoProfileCatalog() {
    return [](const std::string& name) -> std::filesystem::path {
        if (name == "quiet") return std::filesystem::path("/cat/quiet.json");
        if (name == "snd-desk") return std::filesystem::path("/cat/snd-desk.json");
        return {};
    };
}

void TestProfileFlagResolvesToCatalogPath() {
    ProfileResolutionRequest req;
    req.profile_flag = "quiet";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectEqual(res.config_path.generic_string(), "/cat/quiet.json",
                "profile flag resolves to its catalog config path");
    ExpectEqual(res.profile_name, "quiet",
                "profile flag sets the resolved profile name");
    ExpectEqual(SourceLabel(res), "profile_flag", "profile flag resolution source");
}

void TestEnvProfileUsedWhenNoFlag() {
    ProfileResolutionRequest req;
    req.profile_env = "quiet";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectEqual(res.config_path.generic_string(), "/cat/quiet.json",
                "env profile resolves to its catalog config path");
    ExpectEqual(res.profile_name, "quiet", "env profile sets the resolved name");
    ExpectEqual(SourceLabel(res), "profile_env", "env profile resolution source");
}

void TestFlagBeatsEnv() {
    ProfileResolutionRequest req;
    req.profile_flag = "quiet";
    req.profile_env = "snd-desk";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectEqual(res.config_path.generic_string(), "/cat/quiet.json",
                "--profile flag takes precedence over SVG_MB_PROFILE env");
    ExpectEqual(SourceLabel(res), "profile_flag", "flag-beats-env resolution source");
}

void TestMachineIdentityUsedWhenNoFlagOrEnv() {
    ProfileResolutionRequest req;
    req.machine_id = "snd-desk";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectEqual(res.config_path.generic_string(), "/cat/snd-desk.json",
                "machine identity resolves to its catalog config path");
    ExpectEqual(res.profile_name, "snd-desk", "machine identity sets the resolved name");
    ExpectEqual(SourceLabel(res), "machine_identity",
                "machine identity resolution source");
}

void TestExplicitConfigBeatsEverything() {
    ProfileResolutionRequest req;
    req.explicit_config = std::filesystem::path("/x/my.json");
    req.profile_flag = "quiet";
    req.machine_id = "snd-desk";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectEqual(res.config_path.generic_string(), "/x/my.json",
                "explicit --config beats flag / env / machine identity");
    ExpectEqual(res.profile_name, "",
                "explicit --config carries no profile name");
    ExpectEqual(SourceLabel(res), "explicit_config",
                "explicit config resolution source");
}

void TestDefaultWhenNothingSelected() {
    ProfileResolutionRequest req;
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectTrue(res.config_path.empty(),
               "default resolution has an empty config path");
    ExpectEqual(SourceLabel(res), "default", "default resolution source");
}

void TestUnknownMachineIdFallsThroughToDefault() {
    ProfileResolutionRequest req;
    req.machine_id = "unknown-rig";
    const ProfileResolution res = ResolveProfileSelection(req, TwoProfileCatalog());
    ExpectTrue(res.config_path.empty(),
               "unknown machine id falls through to default (empty path)");
    ExpectEqual(SourceLabel(res), "default",
                "unknown machine id resolution source is default");
}

void TestUnknownProfileFlagThrows() {
    ProfileResolutionRequest req;
    req.profile_flag = "nope";
    ExpectThrowsContaining(
        [&] { ResolveProfileSelection(req, TwoProfileCatalog()); }, "nope",
        "an unknown --profile name throws");
}

void TestUnknownProfileEnvThrows() {
    ProfileResolutionRequest req;
    req.profile_env = "nope";
    ExpectThrowsContaining(
        [&] { ResolveProfileSelection(req, TwoProfileCatalog()); }, "nope",
        "an unknown SVG_MB_PROFILE name throws");
}

// ---- Catalog lookup (ResolveProfileConfigPath) ----

using svg_mb_control::ResolveProfileConfigPath;

// Creates a unique temporary directory and removes it (recursively) on destruct.
class TempDir {
  public:
    TempDir() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec || base.empty()) {
            throw std::runtime_error("temp_directory_path failed in TempDir");
        }
        path_ = base / ("svg_mb_control_catalog_test_" + UniqueTempSuffix());
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error("create_directories failed for " +
                                     path_.string());
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path WriteProfile(const std::string& file_name) const {
        const std::filesystem::path p = path_ / file_name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << "{}";
        return p;
    }

  private:
    std::filesystem::path path_;
};

std::string Norm(const std::filesystem::path& p) {
    return std::filesystem::absolute(p).lexically_normal().generic_string();
}

void TestCatalogResolvesExistingProfileFile() {
    TempDir dir;
    const auto expected = dir.WriteProfile("quiet.json");
    const auto got = ResolveProfileConfigPath({dir.path()}, "quiet");
    ExpectEqual(got.generic_string(), Norm(expected),
                "catalog resolves an existing profile to its config file");
}

void TestCatalogReturnsEmptyForMissingProfile() {
    TempDir dir;
    const auto got = ResolveProfileConfigPath({dir.path()}, "missing");
    ExpectTrue(got.empty(), "catalog returns empty for a missing profile");
}

void TestCatalogSearchesDirsInOrder() {
    TempDir first;
    TempDir second;
    const auto expected = second.WriteProfile("snd-desk.json");
    const auto got =
        ResolveProfileConfigPath({first.path(), second.path()}, "snd-desk");
    ExpectEqual(got.generic_string(), Norm(expected),
                "catalog searches directories in order and finds a later match");
}

void TestCatalogRejectsTraversalName() {
    TempDir dir;
    const auto got = ResolveProfileConfigPath({dir.path()}, "../secret");
    ExpectTrue(got.empty(),
               "catalog rejects a profile name containing a path traversal");
}

}  // namespace

int main() {
    TestProfileFlagResolvesToCatalogPath();
    TestEnvProfileUsedWhenNoFlag();
    TestFlagBeatsEnv();
    TestMachineIdentityUsedWhenNoFlagOrEnv();
    TestExplicitConfigBeatsEverything();
    TestDefaultWhenNothingSelected();
    TestUnknownMachineIdFallsThroughToDefault();
    TestUnknownProfileFlagThrows();
    TestUnknownProfileEnvThrows();
    TestCatalogResolvesExistingProfileFile();
    TestCatalogReturnsEmptyForMissingProfile();
    TestCatalogSearchesDirsInOrder();
    TestCatalogRejectsTraversalName();
    if (g_failures == 0) {
        std::cout << "machine_profile_tests: all passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
