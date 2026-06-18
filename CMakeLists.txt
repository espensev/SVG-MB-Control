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
    src/control/boost_stage.cpp
    src/control/cadence_score.cpp
    src/control/calibration.cpp
    src/control/channel_evaluator.cpp
    src/control/channel_write.cpp
    src/control/control_config.cpp
    src/control/control_config_print.cpp
    src/control/control_loop.cpp
    src/control/control_loop_config.cpp
    src/control/control_math.cpp
    src/control/control_runtime_context.cpp
    src/control/control_scheduler.cpp
    src/control/control_status_writer.cpp
    src/control/control_supervisor.cpp
    src/control/low_band_evidence.cpp
    src/control/low_band_integrator.cpp
    src/control/tick_runner.cpp
    src/control/worker_force_terminate.cpp
    src/hardware/amd_reader.cpp
    src/hardware/fan_writer.cpp
    src/hardware/gpu_reader.cpp
    src/hardware/pawnio_binary.cpp
    src/hardware/simulated_fan_writer.cpp
    src/hardware/sio_fan_writer.cpp
    src/platform/direct_runtime_snapshot.cpp
    src/platform/env_util.cpp
    src/platform/file_hash.cpp
    src/platform/runtime_singleton.cpp
    src/platform/runtime_util.cpp
    src/platform/service_probe.cpp
    src/policy/control_policy.cpp
    src/runtime/csv_util.cpp
    src/runtime/evidence_log.cpp
    src/runtime/evidence_signatures.cpp
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
    src/runtime/runtime_status.cpp
    src/runtime/runtime_supervisor_state.cpp
    src/runtime/runtime_write_policy.cpp
    src/runtime/write_orchestrator.cpp
    src/analyze/analyze_channel_sample_columns.cpp
    src/analyze/analyze_cli.cpp
    src/analyze/analyze_csv.cpp
    src/analyze/analyze_db.cpp
    src/analyze/analyze_ingest.cpp
    src/analyze/analyze_ingest_db.cpp
    src/analyze/analyze_json_artifacts.cpp
    src/analyze/analyze_prune.cpp
    src/analyze/analyze_report_data.cpp
    src/analyze/analyze_report_emit.cpp
    src/analyze/analyze_report_queries.cpp
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
    src/app/app_args.cpp
    src/app/app_diagnose.cpp
    src/app/app_main.cpp
    src/app/app_signals.cpp
    src/app/startup_banner.cpp
    src/main.cpp
    ${SVG_MB_CONTROL_RESOURCE_SOURCES}
)

target_link_libraries(svg_mb_control PRIVATE svg_mb_control_core)

set_target_properties(svg_mb_control PROPERTIES OUTPUT_NAME "svg-mb-control")

configure_svg_mb_control_target(svg_mb_control)

if(WIN32)
    add_executable(svg_mb_control_task_runner WIN32
        src/platform/task_runner.cpp
        ${SVG_MB_CONTROL_RESOURCE_SOURCES}
    )
    set_target_properties(svg_mb_control_task_runner PROPERTIES
        OUTPUT_NAME "svg-mb-control-task-runner")
    target_link_libraries(svg_mb_control_task_runner PRIVATE
        nlohmann_json::nlohmann_json)
    configure_svg_mb_control_target(svg_mb_control_task_runner)
endif()

# Throwaway one-shot read-only probe confirming the shipped AMDFamily17 bin
# services the AMD read-only APERF/MPERF aliases (0xC00000E7 / 0xC00000E8) -- the
# corrected FEAT-0006 cycle-counter validation (the 2026-06-07 gate probe read
# the wrong indices). OFF by default: not built by Test-LocalCI / Build-Release /
# CTest, never shipped in release\, not part of the control loop. Build with
#   cmake -DSVG_MB_CONTROL_BUILD_CYCLE_PROBE=ON ... && cmake --build ... \
#         --target cpu_cycle_counter_probe
# Reuses only the bin-load API (pawnio_binary) + env_util; does not link
# svg_mb_control_core and does not touch src/hardware.
option(SVG_MB_CONTROL_BUILD_CYCLE_PROBE
    "Build the throwaway read-only CPU cycle-counter validation probe (FEAT-0006)" OFF)
if(SVG_MB_CONTROL_BUILD_CYCLE_PROBE AND WIN32)
    add_executable(cpu_cycle_counter_probe
        tools/cpu_cycle_counter_probe.cpp
        src/hardware/pawnio_binary.cpp
        src/platform/env_util.cpp
    )
    target_include_directories(cpu_cycle_counter_probe PRIVATE
        ${SVG_MB_CONTROL_INCLUDE_DIRS})
    target_compile_features(cpu_cycle_counter_probe PRIVATE cxx_std_20)
    target_compile_definitions(cpu_cycle_counter_probe PRIVATE
        ${SVG_MB_CONTROL_COMMON_DEFINITIONS})
    if(MSVC)
        target_compile_options(cpu_cycle_counter_probe PRIVATE /W4 /permissive-)
    endif()
    target_link_libraries(cpu_cycle_counter_probe PRIVATE bcrypt)
    set_target_properties(cpu_cycle_counter_probe PROPERTIES
        OUTPUT_NAME "cpu-cycle-counter-probe")
endif()

# Throwaway synthetic CPU load generator for the FEAT-0006 CPU-energy
# quarantine-exit capture: drives a controlled all-core load so a session has a
# clean idle->load->cooldown step. OFF by default: not built by Test-LocalCI /
# Build-Release / CTest, never shipped in release\, not part of the control
# loop. Self-contained (no svg_mb_control_core link, no src/hardware, no MSR).
# Build with
#   cmake -DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON ... && cmake --build ... \
#         --target cpu_synth_load
option(SVG_MB_CONTROL_BUILD_SYNTH_LOAD
    "Build the throwaway synthetic CPU load generator (FEAT-0006 energy capture + Layer-0 stall reproduction)" OFF)
if(SVG_MB_CONTROL_BUILD_SYNTH_LOAD AND WIN32)
    add_executable(cpu_synth_load tools/cpu_synth_load.cpp)
    target_compile_features(cpu_synth_load PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(cpu_synth_load PRIVATE /W4 /permissive- /arch:AVX2)
    endif()
    set_target_properties(cpu_synth_load PROPERTIES
        OUTPUT_NAME "cpu-synth-load")
    # Sibling: constant-occupancy spike load generator for the transient
    # power-anticipation capture (docs/cpu-transient-power-anticipation-plan-2026-06-15.md
    # §8). Same OFF-by-default flag; same self-contained, read-only posture.
    add_executable(cpu_synth_spike_load tools/cpu_synth_spike_load.cpp)
    target_compile_features(cpu_synth_spike_load PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(cpu_synth_spike_load PRIVATE /W4 /permissive- /arch:AVX2)
    endif()
    set_target_properties(cpu_synth_spike_load PROPERTIES
        OUTPUT_NAME "cpu-synth-spike-load")
endif()

if(BUILD_TESTING)
    # Every core-linked C++ test shares identical wiring: one executable built
    # from a tests/cpp/*.cpp, linked against svg_mb_control_core, configured with
    # the shared include/warning/PCH/define settings, and registered with CTest.
    # This helper keeps each registration to a single line and removes the
    # copy-paste-rename footgun (cloning a block but missing one of the name
    # substitutions). Header-only tests that must not link the core library
    # (rapl_energy / cpu_cycles below) stay hand-rolled.
    function(svg_mb_control_add_core_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE svg_mb_control_core)
        configure_svg_mb_control_target(${name})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    svg_mb_control_add_core_test(svg_mb_control_core_tests
        tests/cpp/core_smoke_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_pawnio_binary_tests
        tests/cpp/pawnio_binary_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_boost_stage_tests
        tests/cpp/boost_stage_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_math_tests
        tests/cpp/control_math_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_analyze_report_tests
        tests/cpp/analyze_report_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_loop_config_tests
        tests/cpp/control_loop_config_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_csv_rows_tests
        tests/cpp/csv_rows_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_runtime_event_log_tests
        tests/cpp/runtime_event_log_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_channel_write_tests
        tests/cpp/channel_write_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_force_terminate_tests
        tests/cpp/worker_force_terminate_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_amd_decode_tests
        tests/cpp/amd_decode_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_power_anticipation_tests
        tests/cpp/power_anticipation_tests.cpp)
    svg_mb_control_add_core_test(svg_mb_control_pending_writes_store_tests
        tests/cpp/pending_writes_store_tests.cpp)

    # rapl_energy.h is header-only and dependency-free, so this test does not
    # link svg_mb_control_core (keeps it fast and isolated like the math it covers).
    add_executable(svg_mb_control_rapl_energy_tests
        tests/cpp/rapl_energy_tests.cpp
    )
    target_include_directories(svg_mb_control_rapl_energy_tests PRIVATE
        src/hardware)
    target_compile_features(svg_mb_control_rapl_energy_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(svg_mb_control_rapl_energy_tests PRIVATE
            /W4 /permissive-)
    endif()
    add_test(NAME svg_mb_control_rapl_energy_tests
             COMMAND svg_mb_control_rapl_energy_tests)

    # cpu_cycles.h is header-only and dependency-free, so this test does not link
    # svg_mb_control_core (keeps it fast and isolated like the math it covers).
    add_executable(svg_mb_control_cpu_cycles_tests
        tests/cpp/cpu_cycles_tests.cpp
    )
    target_include_directories(svg_mb_control_cpu_cycles_tests PRIVATE
        src/hardware)
    target_compile_features(svg_mb_control_cpu_cycles_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(svg_mb_control_cpu_cycles_tests PRIVATE
            /W4 /permissive-)
    endif()
    add_test(NAME svg_mb_control_cpu_cycles_tests
             COMMAND svg_mb_control_cpu_cycles_tests)
endif()
