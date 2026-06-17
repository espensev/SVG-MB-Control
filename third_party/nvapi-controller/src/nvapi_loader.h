#ifndef NVAPI_LOADER_H
#define NVAPI_LOADER_H

#include "nvapi_undoc_types.h"

#include <string>

class NvApiLoader {
public:
    NvApiLoader();
    ~NvApiLoader();

    NvApiLoader(const NvApiLoader&) = delete;
    NvApiLoader& operator=(const NvApiLoader&) = delete;

    bool init(std::string& out_warning);
    void shutdown();

    bool is_ready() const { return initialized_; }

    template <typename T>
    T resolve(uint32_t id) const {
        return query_interface_
            ? reinterpret_cast<T>(query_interface_(id))
            : nullptr;
    }

private:
    void* dll_handle_ = nullptr;
    bool initialized_ = false;

    NvAPI_QueryInterface_t query_interface_ = nullptr;
    NvAPI_Initialize_t fn_initialize_ = nullptr;
    NvAPI_Unload_t fn_unload_ = nullptr;
};

#endif /* NVAPI_LOADER_H */
