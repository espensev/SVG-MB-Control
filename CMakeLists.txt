cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" SVG_MB_CONTROL_VERSION_RAW)
string(STRIP "${SVG_MB_CONTROL_VERSION_RAW}" SVG_MB_CONTROL_VERSION)

project(svg_mb_control VERSION ${SVG_MB_CONTROL_VERSION} LANGUAGES C CXX)

include(CTest)

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

set(SVG_MB_CONTROL_COMMON_DEFINITIONS
    SVG_MB_CONTROL_VERSION="${PROJECT_VERSION}"
    SVG_MB_CONTROL_GIT_HASH="${SVG_MB_CONTROL_GIT_HASH}"
)

set(SVG_MB_CONTROL_INCLUDE_DIRS
    src
    src/app
    src/control
    src/hardware
    src/platform
    src/policy
    src/runtime
)

function(configure_svg_mb_control_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${SVG_MB_CONTROL_INCLUDE_DIRS}
    )
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_precompile_headers(${target_name} PRIVATE src/pch.h)
    target_compile_definitions(${target_name} PRIVATE
        ${SVG_MB_CONTROL_COMMON_DEFINITIONS}
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /MP)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(svg_mb_control_core STATIC
    src/pch.cpp
    src/control/cadence_score.cpp
    src/control/calibration.cpp
    src/control/channel_evaluator.cpp
    src/control/channel_write.cpp
    src/control/control_config.cpp
    src/control/control_loop.cpp
    src/control/control_loop_config.cpp
    src/control/control_runtime_context.cpp
    src/control/control_scheduler.cpp
    src/control/control_status_writer.cpp
    src/control/control_supervisor.cpp
    src/control/low_band_evidence.cpp
    src/control/low_band_integrator.cpp
    src/control/tick_runner.cpp
    src/hardware/amd_reader.cpp
    src/hardware/fan_writer.cpp
    src/hardware/gpu_reader.cpp
    src/hardware/simulated_fan_writer.cpp
    src/hardware/sio_fan_writer.cpp
    src/platform/direct_runtime_snapshot.cpp
    src/platform/env_util.cpp
    src/platform/runtime_util.cpp
    src/platform/service_probe.cpp
    src/policy/control_policy.cpp
    src/policy/runtime_write_policy.cpp
    src/policy/write_orchestrator.cpp
    src/runtime/csv_util.cpp
    src/runtime/evidence_log.cpp
    src/runtime/gpu_evidence_csv.cpp
    src/runtime/json_io.cpp
    src/runtime/pending_writes.cpp
    src/runtime/read_loop.cpp
    src/runtime/runtime_csv_archive.cpp
    src/runtime/runtime_event_log.cpp
    src/runtime/runtime_paths.cpp
    src/runtime/runtime_health.cpp
    src/runtime/runtime_csv_rows.cpp
    src/runtime/runtime_lifecycle.cpp
    src/runtime/runtime_snapshot.cpp
    src/runtime/runtime_supervisor_state.cpp
    src/analyze/analyze_cli.cpp
    src/analyze/analyze_csv.cpp
    src/analyze/analyze_db.cpp
    src/analyze/analyze_ingest.cpp
    src/analyze/analyze_ingest_db.cpp
    src/analyze/analyze_json_artifacts.cpp
    src/analyze/analyze_prune.cpp
    src/analyze/analyze_report.cpp
)

target_link_libraries(svg_mb_control_core PUBLIC
    nlohmann_json::nlohmann_json
)

target_link_libraries(svg_mb_control_core PRIVATE
    svg_mb_sio::svg_mb_sio
    sqlite3::sqlite3
)

if(WIN32)
    target_link_libraries(svg_mb_control_core PRIVATE psapi winmm bcrypt)
endif()

if(TARGET gpu_telemetry::gpu_telemetry)
    target_link_libraries(svg_mb_control_core PRIVATE gpu_telemetry::gpu_telemetry)
    target_compile_definitions(svg_mb_control_core PRIVATE SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED=1)
endif()

configure_svg_mb_control_target(svg_mb_control_core)

add_executable(svg_mb_control
    src/app/app_main.cpp
    src/app/startup_banner.cpp
    src/main.cpp
    ${SVG_MB_CONTROL_RESOURCE_SOURCES}
)

target_link_libraries(svg_mb_control PRIVATE svg_mb_control_core)

set_target_properties(svg_mb_control PROPERTIES OUTPUT_NAME "svg-mb-control")

configure_svg_mb_control_target(svg_mb_control)

if(BUILD_TESTING)
    add_executable(svg_mb_control_core_tests
        tests/cpp/core_smoke_tests.cpp
    )
    target_link_libraries(svg_mb_control_core_tests PRIVATE
        svg_mb_control_core
    )
    configure_svg_mb_control_target(svg_mb_control_core_tests)
    add_test(NAME svg_mb_control_core_tests
             COMMAND svg_mb_control_core_tests)
endif()
