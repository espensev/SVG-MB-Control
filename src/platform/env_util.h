#pragma once

#include <string>
#include <string_view>

namespace svg_mb_control {

// Returns the value of environment variable `name`, or `fallback` when the
// variable is unset or empty.
std::string GetEnvOrDefault(const char* name, std::string_view fallback);

}  // namespace svg_mb_control
