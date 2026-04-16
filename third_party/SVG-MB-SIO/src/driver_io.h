#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

namespace mb::hw {

enum class Status {
    ok = 0,
    error = -1,
    invalid_arg = -2,
    not_supported = -4,
    no_device = -5,
    access_denied = -6,
    buffer_too_small = -7,
};

inline const char* status_string(Status status) {
    switch (status) {
        case Status::ok:
            return "ok";
        case Status::error:
            return "error";
        case Status::invalid_arg:
            return "invalid_arg";
        case Status::not_supported:
            return "not_supported";
        case Status::no_device:
            return "no_device";
        case Status::access_denied:
            return "access_denied";
        case Status::buffer_too_small:
            return "buffer_too_small";
        default:
            return "unknown";
    }
}

inline Status status_from_win32_error(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_SERVICE_DOES_NOT_EXIST:
        case ERROR_DEV_NOT_EXIST:
            return Status::no_device;
        case ERROR_ACCESS_DENIED:
            return Status::access_denied;
        case ERROR_NOT_SUPPORTED:
            return Status::not_supported;
        default:
            return Status::error;
    }
}

}  // namespace mb::hw
