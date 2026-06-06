#pragma once

#include <filesystem>
#include <string>

namespace svg_mb_control {

std::string Sha256FileHex(const std::filesystem::path& path);

}  // namespace svg_mb_control
