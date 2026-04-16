cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SVG_MB_SIO_VERSION_RAW)
string(STRIP "${SVG_MB_SIO_VERSION_RAW}" SVG_MB_SIO_VERSION)

project(svg_mb_sio VERSION ${SVG_MB_SIO_VERSION} LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(SVG_MB_SIO_BUILD_TESTING "Build SVG-MB-SIO tests" ${PROJECT_IS_TOP_LEVEL})

add_library(svg_mb_sio STATIC
    src/svg_mb_sio.cpp
    src/fan_sio.cpp
)

target_compile_features(svg_mb_sio PRIVATE cxx_std_20)

target_include_directories(svg_mb_sio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(svg_mb_sio PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(svg_mb_sio PRIVATE /W4 /permissive-)
else()
    target_compile_options(svg_mb_sio PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(svg_mb_sio::svg_mb_sio ALIAS svg_mb_sio)

if(SVG_MB_SIO_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

install(TARGETS svg_mb_sio
    EXPORT svg_mb_sioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/svg_mb_sio
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT svg_mb_sioTargets
    FILE svg_mb_sioTargets.cmake
    NAMESPACE svg_mb_sio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/svg_mb_sio
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/svg_mb_sioConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/svg_mb_sioConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/svg_mb_sio
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/svg_mb_sioConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/svg_mb_sioConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/svg_mb_sioConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/svg_mb_sio
)
