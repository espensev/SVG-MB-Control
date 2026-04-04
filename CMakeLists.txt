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

add_executable(svg_mb_control
    src/bench_bridge.cpp
    src/main.cpp
)

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
