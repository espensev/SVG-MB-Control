#pragma once

// Centralized lean Windows.h include. Use this header everywhere a TU
// needs <windows.h> so the WIN32_LEAN_AND_MEAN / NOMINMAX guards are
// defined consistently before including the Win32 headers.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
