#include "machine_profile.h"

#include <stdexcept>
#include <system_error>
#include <utility>

namespace svg_mb_control {

std::string_view ProfileResolutionSourceLabel(ProfileResolutionSource source) {
    switch (source) {
        case ProfileResolutionSource::kExplicitConfig:
            return "explicit_config";
        case ProfileResolutionSource::kProfileFlag:
            return "profile_flag";
        case ProfileResolutionSource::kProfileEnv:
            return "profile_env";
        case ProfileResolutionSource::kMachineIdentity:
            return "machine_identity";
        case ProfileResolutionSource::kDefault:
            return "default";
    }
    return "default";
}

ProfileResolution ResolveProfileSelection(
    const ProfileResolutionRequest& request,
    const ProfileCatalogLookup& lookup) {
    // 1. Explicit --config wins; profile resolution is skipped entirely.
    if (!request.explicit_config.empty()) {
        return ProfileResolution{request.explicit_config, std::string(),
                                 ProfileResolutionSource::kExplicitConfig};
    }
    // 2. --profile flag: an explicit selection, so an unknown name is an error.
    if (!request.profile_flag.empty()) {
        std::filesystem::path path = lookup(request.profile_flag);
        if (path.empty()) {
            throw std::runtime_error("Unknown profile (--profile): " +
                                     request.profile_flag);
        }
        return ProfileResolution{std::move(path), request.profile_flag,
                                 ProfileResolutionSource::kProfileFlag};
    }
    // 3. SVG_MB_PROFILE env: also an explicit selection; unknown name is an error.
    if (!request.profile_env.empty()) {
        std::filesystem::path path = lookup(request.profile_env);
        if (path.empty()) {
            throw std::runtime_error("Unknown profile (SVG_MB_PROFILE): " +
                                     request.profile_env);
        }
        return ProfileResolution{std::move(path), request.profile_env,
                                 ProfileResolutionSource::kProfileEnv};
    }
    // 4. Machine identity: best-effort; an unknown machine falls through to the
    //    default so a new rig never fails to start.
    if (!request.machine_id.empty()) {
        std::filesystem::path path = lookup(request.machine_id);
        if (!path.empty()) {
            return ProfileResolution{std::move(path), request.machine_id,
                                     ProfileResolutionSource::kMachineIdentity};
        }
    }
    // 5. Default chain (empty config_path; the caller resolves the built-in
    //    default).
    return ProfileResolution{};
}

std::filesystem::path ResolveProfileConfigPath(
    const std::vector<std::filesystem::path>& profile_dirs,
    const std::string& profile_name) {
    // Reject anything that is not a bare catalog name, so a profile name can
    // never traverse out of the catalog directories.
    if (profile_name.empty() ||
        profile_name.find('/') != std::string::npos ||
        profile_name.find('\\') != std::string::npos ||
        profile_name.find("..") != std::string::npos) {
        return {};
    }
    for (const auto& dir : profile_dirs) {
        if (dir.empty()) {
            continue;
        }
        const std::filesystem::path candidate = dir / (profile_name + ".json");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }
    return {};
}

}  // namespace svg_mb_control
