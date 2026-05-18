cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" SVG_MB_CONTROL_VERSION_RAW)
string(STRIP "${SVG_MB_CONTROL_VERSION_RAW}" SVG_MB_CONTROL_VERSION)

project(svg_mb_control VERSION ${SVG_MB_CONTROL_VERSION} LANGUAGES C CXX)

set(SVG_MB_CONTROL_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(SVG_MB_CONTROL_NLOHMANN_JSON_INCLUDE_DIR
    "${SVG_MB_CONTROL_THIRD_PARTY_DIR}/nlohmann-json/include")
if(NOT EXISTS "${SVG_MB_CONTROL_NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR
            "Vendored nlohmann/json header not found at "
            "${SVG_MB_CONTROL_NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
endif()

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    "${SVG_MB_CONTROL_NLOHMANN_JSON_INCLUDE_DIR}")

set(SVG_MB_CONTROL_SQLITE3_DIR
    "${SVG_MB_CONTROL_THIRD_PARTY_DIR}/sqlite3")
if(NOT EXISTS "${SVG_MB_CONTROL_SQLITE3_DIR}/sqlite3.c"
   OR NOT EXISTS "${SVG_MB_CONTROL_SQLITE3_DIR}/sqlite3.h")
    message(FATAL_ERROR
            "Vendored sqlite3 amalgamation not found at "
            "${SVG_MB_CONTROL_SQLITE3_DIR}")
endif()

add_library(sqlite3_amalgamation STATIC
    "${SVG_MB_CONTROL_SQLITE3_DIR}/sqlite3.c")
add_library(sqlite3::sqlite3 ALIAS sqlite3_amalgamation)
set_target_properties(sqlite3_amalgamation PROPERTIES LINKER_LANGUAGE C)
target_include_directories(sqlite3_amalgamation PUBLIC
    "${SVG_MB_CONTROL_SQLITE3_DIR}")
if(MSVC)
    target_compile_options(sqlite3_amalgamation PRIVATE /W0)
endif()

# Always build against the vendored source trees in this repo.
set(SVG_MB_SIO_SOURCE_DIR
    "${SVG_MB_CONTROL_THIRD_PARTY_DIR}/SVG-MB-SIO")
if(NOT EXISTS "${SVG_MB_SIO_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "Vendored SVG-MB-SIO source tree not found at ${SVG_MB_SIO_SOURCE_DIR}")
endif()

if(NOT TARGET svg_mb_sio::svg_mb_sio)
    set(SVG_MB_SIO_BUILD_TESTING OFF CACHE BOOL "Build vendored SVG-MB-SIO tests" FORCE)
    add_subdirectory("${SVG_MB_SIO_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/_deps/svg_mb_sio")
endif()

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
set(SVG_MB_CONTROL_NVAPI_CONTROLLER_ROOT
    "${SVG_MB_CONTROL_THIRD_PARTY_DIR}/nvapi-controller")
set(SVG_MB_CONTROL_GPU_TELEMETRY_SOURCE_DIR
    "${SVG_MB_CONTROL_NVAPI_CONTROLLER_ROOT}/telemetry")

if(SVG_MB_CONTROL_ENABLE_GPU)
    if(EXISTS "${SVG_MB_CONTROL_GPU_TELEMETRY_SOURCE_DIR}/CMakeLists.txt")
        if(NOT TARGET gpu_telemetry::gpu_telemetry)
            add_subdirectory("${SVG_MB_CONTROL_GPU_TELEMETRY_SOURCE_DIR}"
                             "${CMAKE_CURRENT_BINARY_DIR}/_deps/gpu_telemetry")
        endif()
    else()
        message(WARNING "Vendored gpu_telemetry source not found at "
                        "${SVG_MB_CONTROL_GPU_TELEMETRY_SOURCE_DIR}; GPU input disabled.")
    endif()
endif()

set(SVG_MB_CONTROL_RESOURCE_SOURCES)
if(WIN32)
    enable_language(RC)
    list(APPEND SVG_MB_CONTROL_RESOURCE_SOURCES
        resources/svg_mb_control.rc
    )
endif()

add_executable(svg_mb_control
    src/pch.cpp
    src/amd_reader.cpp
    src/analyze/analyze_csv.cpp
    src/analyze/analyze_db.cpp
    src/analyze/analyze_ingest.cpp
    src/analyze/analyze_ingest_db.cpp
    src/analyze/analyze_json_artifacts.cpp
    src/calibration.cpp
    src/channel_evaluator.cpp
    src/control_config.cpp
    src/control_loop.cpp
    src/control_loop_config.cpp
    src/control_policy.cpp
    src/control_runtime_context.cpp
    src/control_scheduler.cpp
    src/control_status_writer.cpp
    src/control_supervisor.cpp
    src/csv_util.cpp
    src/direct_runtime_snapshot.cpp
    src/evidence_log.cpp
    src/fan_writer.cpp
    src/gpu_evidence_csv.cpp
    src/gpu_reader.cpp
    src/json_io.cpp
    src/main.cpp
    src/pending_writes.cpp
    src/read_loop.cpp
    src/runtime_artifacts.cpp
    src/runtime_health.cpp
    src/runtime_logging.cpp
    src/runtime_lifecycle.cpp
    src/runtime_snapshot.cpp
    src/runtime_supervisor_state.cpp
    src/runtime_write_policy.cpp
    src/simulated_fan_writer.cpp
    src/sio_fan_writer.cpp
    src/write_orchestrator.cpp
    ${SVG_MB_CONTROL_RESOURCE_SOURCES}
)

target_link_libraries(svg_mb_control PRIVATE
    svg_mb_sio::svg_mb_sio
    nlohmann_json::nlohmann_json
    sqlite3::sqlite3
)

if(WIN32)
    target_link_libraries(svg_mb_control PRIVATE psapi winmm)
endif()

if(TARGET gpu_telemetry::gpu_telemetry)
    target_link_libraries(svg_mb_control PRIVATE gpu_telemetry::gpu_telemetry)
    target_compile_definitions(svg_mb_control PRIVATE SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED=1)
endif()

set_target_properties(svg_mb_control PROPERTIES OUTPUT_NAME "svg-mb-control")

target_include_directories(svg_mb_control PRIVATE src)
target_compile_features(svg_mb_control PRIVATE cxx_std_20)

# Enable precompiled headers for faster builds
target_precompile_headers(svg_mb_control PRIVATE src/pch.h)

target_compile_definitions(svg_mb_control PRIVATE
    SVG_MB_CONTROL_VERSION="${PROJECT_VERSION}"
    SVG_MB_CONTROL_GIT_HASH="${SVG_MB_CONTROL_GIT_HASH}"
)

if(MSVC)
    target_compile_options(svg_mb_control PRIVATE /W4 /permissive- /MP)
else()
    target_compile_options(svg_mb_control PRIVATE -Wall -Wextra -Wpedantic)
endif()
