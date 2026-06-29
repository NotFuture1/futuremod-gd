cmake_minimum_required(VERSION 3.21)

# Build for both Apple Silicon and Intel on macOS.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")

project(FutureMod VERSION 1.0.0)

# Collect every .cpp under src/
file(GLOB_RECURSE SOURCES
    src/*.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Locate the Geode SDK via the GEODE_SDK environment variable.
if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Set the GEODE_SDK environment variable (run: geode sdk install).")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
