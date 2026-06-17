#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "nvapi_loader.h"

NvApiLoader::NvApiLoader() = default;

NvApiLoader::~NvApiLoader() {
    shutdown();
}

bool NvApiLoader::init(std::string& out_warning) {
    if (initialized_) {
        return true;
    }

    dll_handle_ = LoadLibraryW(L"nvapi64.dll");
    if (!dll_handle_) {
        out_warning = "Unable to load nvapi64.dll. Ensure an NVIDIA driver is installed.";
        return false;
    }

    query_interface_ = reinterpret_cast<NvAPI_QueryInterface_t>(
        GetProcAddress(static_cast<HMODULE>(dll_handle_), "nvapi_QueryInterface"));
    if (!query_interface_) {
        out_warning = "nvapi_QueryInterface not found in nvapi64.dll.";
        shutdown();
        return false;
    }

    fn_initialize_ = resolve<NvAPI_Initialize_t>(NVAPI_ID_INITIALIZE);
    fn_unload_ = resolve<NvAPI_Unload_t>(NVAPI_ID_UNLOAD);
    if (!fn_initialize_ || !fn_unload_) {
        out_warning = "Required NVAPI loader entry points are not available.";
        shutdown();
        return false;
    }

    NvAPI_Status st = fn_initialize_();
    if (st != NVAPI_OK) {
        out_warning = "NvAPI_Initialize failed (status " + std::to_string(st) + ").";
        shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void NvApiLoader::shutdown() {
    if (initialized_ && fn_unload_) {
        fn_unload_();
    }

    initialized_ = false;
    fn_initialize_ = nullptr;
    fn_unload_ = nullptr;
    query_interface_ = nullptr;

    if (dll_handle_) {
        FreeLibrary(static_cast<HMODULE>(dll_handle_));
        dll_handle_ = nullptr;
    }
}
