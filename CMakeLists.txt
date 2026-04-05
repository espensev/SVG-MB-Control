cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" SVG_MB_CONTROL_VERSION_RAW)
string(STRIP "${SVG_MB_CONTROL_VERSION_RAW}" SVG_MB_CONTROL_VERSION)

project(svg_mb_control VERSION ${SVG_MB_CONTROL_VERSION} LANGUAGES CXX)

set(SVG_MB_CONTROL_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
        RESULT_VARIABLE _git_result
        OUTPUT_VARIABLE _git_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_result EQUAL 0 AND NOT _git_output STREQUAL "")
        set(SVG_MB_CONTROL_GIT_HASH "${_git_output}")
    endif()
endif()

option(SVG_MB_CONTROL_ENABLE_GPU "Link nvapi-controller gpu_telemetry for GPU input" ON)
set(SVG_MB_CONTROL_GPU_TELEMETRY_ROOT "$ENV{GPU_TELEMETRY_ROOT}" CACHE PATH
    "Path to nvapi-controller gpu_telemetry install package (lib/cmake/gpu_telemetry parent)")
if(NOT SVG_MB_CONTROL_GPU_TELEMETRY_ROOT)
    set(SVG_MB_CONTROL_GPU_TELEMETRY_ROOT
        "${CMAKE_SOURCE_DIR}/../../Nvida-fancontrol-unofficial/nvapi-controller/build/install/telemetry-package-x64")
endif()

if(SVG_MB_CONTROL_ENABLE_GPU)
    find_package(gpu_telemetry QUIET
        PATHS "${SVG_MB_CONTROL_GPU_TELEMETRY_ROOT}"
        NO_DEFAULT_PATH
    )
    if(NOT gpu_telemetry_FOUND)
        message(WARNING "gpu_telemetry not found at ${SVG_MB_CONTROL_GPU_TELEMETRY_ROOT}; "
                        "GPU input disabled. Set GPU_TELEMETRY_ROOT env var or "
                        "-DSVG_MB_CONTROL_GPU_TELEMETRY_ROOT=<path> to enable.")
    endif()
endif()

add_executable(svg_mb_control
    src/bench_bridge.cpp
    src/control_config.cpp
    src/control_loop.cpp
    src/control_policy.cpp
    src/gpu_reader.cpp
    src/main.cpp
    src/pending_writes.cpp
    src/read_loop.cpp
    src/write_orchestrator.cpp
)

if(gpu_telemetry_FOUND)
    target_link_libraries(svg_mb_control PRIVATE gpu_telemetry::gpu_telemetry)
    target_compile_definitions(svg_mb_control PRIVATE SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED=1)
endif()

set_target_properties(svg_mb_control PROPERTIES OUTPUT_NAME "svg-mb-control")

target_include_directories(svg_mb_control PRIVATE src)
target_compile_features(svg_mb_control PRIVATE cxx_std_20)
target_compile_definitions(svg_mb_control PRIVATE
    SVG_MB_CONTROL_VERSION="${PROJECT_VERSION}"
    SVG_MB_CONTROL_GIT_HASH="${SVG_MB_CONTROL_GIT_HASH}"
)

if(MSVC)
    target_compile_options(svg_mb_control PRIVATE /W4 /permissive-)
else()
    target_compile_options(svg_mb_control PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(fake_bench
    tests/fake_bench.cpp
)

set_target_properties(fake_bench PROPERTIES OUTPUT_NAME "fake-bench")

target_compile_features(fake_bench PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(fake_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(fake_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
