#ifndef NVAPI_UNDOC_IDS_H
#define NVAPI_UNDOC_IDS_H

#include <cstdint>

/* ---- function IDs (for nvapi_QueryInterface) ---- */

constexpr uint32_t NVAPI_ID_INITIALIZE = 0x0150E828;
constexpr uint32_t NVAPI_ID_UNLOAD = 0xD22BDD7E;
constexpr uint32_t NVAPI_ID_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
constexpr uint32_t NVAPI_ID_GPU_GET_FULL_NAME = 0xCEEE8E9F;
constexpr uint32_t NVAPI_ID_GPU_CLIENT_FAN_COOLERS_GET_CONTROL = 0x814B209F;
constexpr uint32_t NVAPI_ID_GPU_CLIENT_FAN_COOLERS_GET_STATUS = 0x35AED5E8;
constexpr uint32_t NVAPI_ID_GPU_CLIENT_FAN_COOLERS_SET_CONTROL = 0xA58971A5;
constexpr uint32_t NVAPI_ID_GPU_GET_THERMAL_SENSORS_UNDOC = 0x65FE3AAD;
constexpr uint32_t NVAPI_ID_GPU_GET_THERMAL_SETTINGS = 0xE3640A56;
constexpr uint32_t NVAPI_ID_GPU_GET_BUS_ID = 0x1BE0B8E5;

/* ---- constants ---- */

constexpr int NVAPI_MAX_PHYSICAL_GPUS = 64;
constexpr int NVAPI_SHORT_STRING_MAX = 64;
constexpr int NVAPI_MAX_FAN_CONTROLLER_ITEMS = 32;
constexpr int NVAPI_MAX_FAN_STATUS_ITEMS = 32;
constexpr int NVAPI_FAN_CONTROL_ITEM_RESERVED = 8;
constexpr int NVAPI_FAN_CONTROL_RESERVED2 = 8;
constexpr int NVAPI_FAN_STATUS_ITEM_RESERVED = 8;
constexpr int NVAPI_UNDOC_THERMAL_VALUES = 32;

#endif /* NVAPI_UNDOC_IDS_H */
