cmake_minimum_required(VERSION 3.24)
project(TexturePlaceholderMemoryPatch LANGUAGES CXX)

add_library(TexturePlaceholderMemoryPatch SHARED
    src/main.cpp
    extern/plugin.h
)

target_compile_features(TexturePlaceholderMemoryPatch PRIVATE cxx_std_20)
target_compile_definitions(TexturePlaceholderMemoryPatch PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
target_include_directories(TexturePlaceholderMemoryPatch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(CLEAN_OUTPUT_DIR "E:/memory/builds/TexturePlaceholderMemoryPatch")

set_target_properties(TexturePlaceholderMemoryPatch PROPERTIES
    OUTPUT_NAME "TexturePlaceholderMemoryPatch"
    RUNTIME_OUTPUT_DIRECTORY "${CLEAN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CLEAN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CLEAN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CLEAN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CLEAN_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CLEAN_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CLEAN_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
    PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/pdb"
)
