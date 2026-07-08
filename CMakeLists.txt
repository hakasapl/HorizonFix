cmake_minimum_required(VERSION 3.21)

set(VCPKG_TARGET_TRIPLET "x64-windows-static")

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# One source tree, two runtime flavors: configure with -DSKYRIM_SUPPORT_AE=ON
# (default) for AE 1.6.x, or OFF for SE 1.5.97. CommonLibSSE selects address
# library IDs and struct layouts from the same flag at compile time. The two
# flavors get distinct plugin/log/DLL names so they can coexist; a runtime
# version guard in main.cpp makes a wrong-flavor DLL exit cleanly.
option(SKYRIM_SUPPORT_AE "Build for Skyrim AE (1.6.x); OFF builds for SE (1.5.97)" ON)

if(SKYRIM_SUPPORT_AE)
    set(PLUGIN_NAME "HorizonFix")
else()
    set(PLUGIN_NAME "HorizonFixSE")
endif()
set(PLUGIN_VERSION "0.1.0")
project(${PLUGIN_NAME} VERSION ${PLUGIN_VERSION} LANGUAGES CXX)

if(MSVC)
    # Match vcpkg's x64-windows-static triplet (/MT, /MTd) across all targets.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

add_compile_definitions(PLUGIN_NAME="${PLUGIN_NAME}")
add_compile_definitions(PLUGIN_VERSION="${PLUGIN_VERSION}")

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("extern/CommonLibSSE" commonlibsse)

set(headers
    "include/ConfigLoader.hpp"
    "include/PCH.h"
    "include/SkirtCull.hpp"
    "include/SkirtDepth.hpp"
    "include/WaterSkirt.hpp"
)

set(sources
    "src/ConfigLoader.cpp"
    "src/main.cpp"
    "src/SkirtCull.cpp"
    "src/SkirtDepth.cpp"
    "src/WaterSkirt.cpp"
)

add_library(${PROJECT_NAME} SHARED ${headers} ${sources})

add_compile_definitions(NOMINMAX)
add_compile_definitions(WIN32_LEAN_AND_MEAN)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE "include/PCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE "include")

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()
