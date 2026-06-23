#pragma once

// FEAT-0023 profile selection. Pure precedence logic: given the explicit
// --config path, the --profile flag, the SVG_MB_PROFILE env value, and the
// resolved machine id, choose which control-config path to load and record how
// it was chosen. The catalog lookup (profile name -> config path) is injected so
// the precedence is unit-testable without a filesystem or host-name dependency.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace svg_mb_control {

// How the active config path was chosen, in precedence order (highest first).
enum class ProfileResolutionSource {
    kExplicitConfig,   // --config <path> given; profile resolution skipped
    kProfileFlag,      // --profile <name>
    kProfileEnv,       // SVG_MB_PROFILE
    kMachineIdentity,  // resolved from the host / machine id
    kDefault,          // no selector matched; fall through to the default chain
};

std::string_view ProfileResolutionSourceLabel(ProfileResolutionSource source);

struct ProfileResolutionRequest {
    std::filesystem::path explicit_config;  // from --config (may be empty)
    std::string profile_flag;               // from --profile (may be empty)
    std::string profile_env;                // from SVG_MB_PROFILE (may be empty)
    std::string machine_id;                 // resolved host / machine id (may be empty)
};

// Maps a profile name to its config path. Returns an empty path when the name is
// not in the catalog. Injected for testability; the production lookup resolves
// config/profiles/<name>.json under the config search roots.
using ProfileCatalogLookup =
    std::function<std::filesystem::path(const std::string& profile_name)>;

struct ProfileResolution {
    std::filesystem::path config_path;  // empty => caller falls back to default
    std::string profile_name;           // resolved profile name ("" when none)
    ProfileResolutionSource source = ProfileResolutionSource::kDefault;
};

// Resolves the active config path by precedence, highest first:
//   explicit --config > --profile flag > SVG_MB_PROFILE env > machine id > default.
// A non-empty profile_flag or profile_env that the catalog does not contain
// throws std::runtime_error (the operator named a profile that does not exist).
// A machine_id the catalog does not contain falls through to the default
// (config_path empty, source kDefault), so an unknown machine never fails to start.
ProfileResolution ResolveProfileSelection(const ProfileResolutionRequest& request,
                                          const ProfileCatalogLookup& lookup);

// Resolves a profile name to its config file by searching the given profile
// directories in order for "<name>.json", returning the first match as an
// absolute, normalized path. Returns an empty path when no directory contains
// the file, or when the name is empty or contains a path separator or ".." (so
// a profile name can never traverse out of the catalog directories). The
// production caller builds the directory list from the same exe-relative / cwd
// roots used by ResolveDefaultControlConfigPath; the lookup injected into
// ResolveProfileSelection wraps this with the resolved directories.
std::filesystem::path ResolveProfileConfigPath(
    const std::vector<std::filesystem::path>& profile_dirs,
    const std::string& profile_name);

// Returns the directories searched in order for a named profile config:
// <root>/profiles and <root>/config/profiles under the exe-relative and cwd
// roots. Used by startup resolution (app) and by live-switch candidate
// validation (supervisor), so it lives in this shared translation unit.
std::vector<std::filesystem::path> DefaultProfileCatalogDirs();

}  // namespace svg_mb_control
