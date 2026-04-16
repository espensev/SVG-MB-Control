cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GPU_TELEMETRY_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${GPU_TELEMETRY_VERSION_STRING}" GPU_TELEMETRY_VERSION_STRING)

project(gpu_telemetry VERSION ${GPU_TELEMETRY_VERSION_STRING} LANGUAGES CXX RC)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GPU_TELEMETRY_LOWLEVEL_SOURCES
    ../src/nvapi_loader.cpp
    ../src/nvapi_fans.cpp
    ../src/nvapi_thermals.cpp
)

set(GPU_TELEMETRY_NVML_SOURCES
    ../src/nvml_loader.cpp
)

if(TARGET nvapi_controller_lowlevel)
    set(GPU_TELEMETRY_LOWLEVEL_SOURCES
        $<TARGET_OBJECTS:nvapi_controller_lowlevel>
    )
    set(GPU_TELEMETRY_NVML_SOURCES)
endif()

add_library(gpu_telemetry STATIC
    src/gpu_sensor_reader.cpp
    src/gpu_probe.cpp
    ${GPU_TELEMETRY_NVML_SOURCES}
    ${GPU_TELEMETRY_LOWLEVEL_SOURCES}
)

add_library(gpu_telemetry::gpu_telemetry ALIAS gpu_telemetry)

set(GPU_TELEMETRY_C_DLL_SOURCES
    src/gpu_telemetry_c.cpp
)

if(WIN32)
    string(REPLACE "." "," GPU_TELEMETRY_VERSION_COMMA "${PROJECT_VERSION}.0")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gpu_telemetry_c.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetry_c.rc"
        @ONLY
    )
    list(APPEND GPU_TELEMETRY_C_DLL_SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetry_c.rc"
    )
endif()

add_library(gpu_telemetry_c SHARED
    ${GPU_TELEMETRY_C_DLL_SOURCES}
)

add_library(gpu_telemetry::gpu_telemetry_c ALIAS gpu_telemetry_c)

target_include_directories(gpu_telemetry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/gpu_telemetry
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_definitions(gpu_telemetry PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(gpu_telemetry PUBLIC kernel32.lib)

target_include_directories(gpu_telemetry_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(gpu_telemetry_c PRIVATE gpu_telemetry)

target_compile_definitions(gpu_telemetry_c
    PRIVATE
        GPU_TELEMETRY_CAPI_BUILD
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(gpu_telemetry PRIVATE /W3 /sdl /EHsc /permissive-)
    target_compile_options(gpu_telemetry_c PRIVATE /W3 /sdl /EHsc /permissive-)
endif()

set(GPU_TELEMETRY_PUBLIC_HEADERS
    include/gpu_telemetry/gpu_telemetry_c.h
    include/gpu_telemetry/gpu_sensor_reader.h
    include/gpu_telemetry/gpu_probe.h
    include/gpu_telemetry/gpu_snapshot.h
    include/gpu_telemetry/nvml_loader.h
)

install(TARGETS gpu_telemetry gpu_telemetry_c
    EXPORT gpu_telemetryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${GPU_TELEMETRY_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gpu_telemetry
)

install(FILES VERSION README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gpu_telemetry
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gpu_telemetryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetryConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gpu_telemetry"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetryConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT gpu_telemetryTargets
    FILE gpu_telemetryTargets.cmake
    NAMESPACE gpu_telemetry::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gpu_telemetry"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetryConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_telemetryConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gpu_telemetry"
)
