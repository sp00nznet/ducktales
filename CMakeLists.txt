cmake_minimum_required(VERSION 3.20)
project(ducktales_recomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "DuckTales: Remastered - static recompilation (BLUS31368)"
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DUCK_GAME_DIR_OPT "${CMAKE_CURRENT_SOURCE_DIR}/game" CACHE PATH
    "Path to game data directory (holds EBOOT.elf)")

# ---------------------------------------------------------------------------
# ps3recomp SDK
# ---------------------------------------------------------------------------
if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ps3" ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

set(PS3RECOMP_RUNTIME_DIR "${PS3RECOMP_DIR}/build" CACHE PATH
    "Directory containing the pre-built ps3recomp_runtime library")

set(_PS3R_LIB_CANDIDATES
    "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/Release/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/Debug/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a"
)
set(_PS3R_FOUND_LIB "")
foreach(_lib ${_PS3R_LIB_CANDIDATES})
    if(EXISTS "${_lib}")
        set(_PS3R_FOUND_LIB "${_lib}")
        break()
    endif()
endforeach()

if(_PS3R_FOUND_LIB)
    message(STATUS "Found ps3recomp runtime: ${_PS3R_FOUND_LIB}")
    add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
    set_target_properties(ps3recomp_runtime PROPERTIES
        IMPORTED_LOCATION "${_PS3R_FOUND_LIB}")
    if(EXISTS "${PS3RECOMP_RUNTIME_DIR}/Release/ps3recomp_runtime.lib")
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION_RELEASE "${PS3RECOMP_RUNTIME_DIR}/Release/ps3recomp_runtime.lib")
    endif()
    if(EXISTS "${PS3RECOMP_RUNTIME_DIR}/Debug/ps3recomp_runtime.lib")
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION_DEBUG "${PS3RECOMP_RUNTIME_DIR}/Debug/ps3recomp_runtime.lib")
    endif()
    target_include_directories(ps3recomp_runtime INTERFACE "${PS3RECOMP_DIR}/include")
else()
    message(STATUS "Building ps3recomp runtime from source")
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Recompiled game code (49 chunks, ~2 GB of generated C++)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RECOMP_SOURCES "src/recomp/*.cpp")
list(LENGTH RECOMP_SOURCES _n_recomp)
message(STATUS "Recompiled chunks: ${_n_recomp}")

add_executable(ducktales
    src/main.cpp
    src/elf_loader.cpp
    src/vm_bridge.cpp
    src/hle_dispatch.cpp
    src/indirect_dispatch.cpp
    src/import_table.cpp
    src/missing_stubs.cpp
    ${RECOMP_SOURCES}
)

target_include_directories(ducktales PRIVATE
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
)

target_link_libraries(ducktales PRIVATE ps3recomp_runtime)

target_compile_definitions(ducktales PRIVATE
    DUCK_GAME_DIR="${DUCK_GAME_DIR_OPT}"
)

if(WIN32)
    target_link_libraries(ducktales PRIVATE ws2_32 xinput ole32 bcrypt)
endif()

if(MSVC)
    target_compile_options(ducktales PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996 /wd4013 /Zc:__cplusplus /bigobj)
    # Deep split-function call chains need a large native stack.
    target_link_options(ducktales PRIVATE /STACK:0x40000000)
    target_compile_definitions(ducktales PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ducktales PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -fno-strict-aliasing)
endif()

install(TARGETS ducktales RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "DuckTales recomp v${PROJECT_VERSION}")
message(STATUS "  Game dir:  ${DUCK_GAME_DIR_OPT}")
message(STATUS "  ps3recomp: ${PS3RECOMP_DIR}")
message(STATUS "")
