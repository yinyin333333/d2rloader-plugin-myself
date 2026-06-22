cmake_minimum_required(VERSION 3.24)
project(monsterhp_normal_only_probe LANGUAGES CXX)

add_library(monsterhp_normal_only_probe SHARED
    src/main.cpp
    extern/plugin.h
)

target_compile_features(monsterhp_normal_only_probe PRIVATE cxx_std_20)
target_compile_definitions(monsterhp_normal_only_probe PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
target_include_directories(monsterhp_normal_only_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(RELEASE_OUTPUT_DIR "E:/memory/builds/monsterhp_normal_only_probe")

set_target_properties(monsterhp_normal_only_probe PROPERTIES
    OUTPUT_NAME "monsterhp_normal_only_probe"
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RELEASE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RELEASE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${RELEASE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${RELEASE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RELEASE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${RELEASE_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
    PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/pdb"
)
