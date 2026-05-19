#include "env_util.h"

#include <cstddef>
#include <cstdlib>

namespace svg_mb_control {

std::string GetEnvOrDefault(const char* name, std::string_view fallback) {
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr ||
        value[0] == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::string(fallback);
    }
    std::string result(value);
    std::free(value);
    return result;
}

}  // namespace svg_mb_control
