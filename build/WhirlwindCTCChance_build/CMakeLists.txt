cmake_minimum_required(VERSION 3.24)
project(WhirlwindCTCChance LANGUAGES CXX)

add_library(WhirlwindCTCChance SHARED
    src/main.cpp
    extern/plugin.h
)

target_compile_features(WhirlwindCTCChance PRIVATE cxx_std_20)
target_compile_definitions(WhirlwindCTCChance PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WWCTC_DEBUG_LOG=0
)
target_include_directories(WhirlwindCTCChance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(WhirlwindCTCChance PROPERTIES
    OUTPUT_NAME "WhirlwindCTCChance"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
)
