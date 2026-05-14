cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match Prisma's static CRT — fmt/spdlog from its vcpkg_installed are
# x64-windows-static (MT), so we must link MT too or get LNK2038.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

project(PrismaUIAddItemMenu VERSION 0.1.0 LANGUAGES CXX)

# ─── Reuse Prisma's already-installed vcpkg packages ──────────────────────────
# Mirrors the pattern PrismaVR-TestMod uses: no separate vcpkg install needed
# for this project. fmt + spdlog land here.
set(PRISMA_BUILD_ROOT "C:/Users/borja/Desktop/Prisma UI SRC")
set(VCPKG_INSTALLED_DIR "${PRISMA_BUILD_ROOT}/build/vcpkg_installed" CACHE STRING "" FORCE)
list(APPEND CMAKE_PREFIX_PATH "${PRISMA_BUILD_ROOT}/build/vcpkg_installed/x64-windows-static/share")

# CommonLibSSE-NG's CMake checks for RAPIDCSV_INCLUDE_DIRS to be set; it does
# not honor CMAKE_PREFIX_PATH for that one. Point it at the headers directly.
set(RAPIDCSV_INCLUDE_DIRS
    "${PRISMA_BUILD_ROOT}/build/vcpkg_installed/x64-windows-static/include"
    CACHE PATH "" FORCE)

# ─── CommonLibSSE-NG (local checkout, built once and cached) ─────────────────
set(CommonLibPath "C:/Users/borja/Desktop/CommonLibVR-4.1.3")
set(CommonLibName "CommonLibSSE")
set(BUILD_ROOT "${CMAKE_BINARY_DIR}")

if(NOT EXISTS "${CommonLibPath}/vcpkg.json")
    message(FATAL_ERROR "CommonLibSSE-NG not found at ${CommonLibPath}")
endif()

file(READ "${CommonLibPath}/vcpkg.json" _vcpkg_json)
string(JSON COMMONLIBSSE_VERSION GET "${_vcpkg_json}" "version-semver")
set(COMMONLIBSSE_VERSION "${COMMONLIBSSE_VERSION}" CACHE STRING "" FORCE)
message(STATUS "Configuring CommonLibSSE-NG version ${COMMONLIBSSE_VERSION}")

# Always build CommonLibSSE in Release (assertions off, optimizations on).
set(_saved_build_type "${CMAKE_BUILD_TYPE}")
set(CMAKE_BUILD_TYPE "Release")
add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNDEBUG)
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
add_subdirectory("${CommonLibPath}" "${BUILD_ROOT}/external_builds/${CommonLibName}" EXCLUDE_FROM_ALL)

include("${CommonLibPath}/cmake/CommonLibSSE.cmake")

set(CMAKE_BUILD_TYPE "${_saved_build_type}")
add_compile_definitions(COMMONLIBSSE_VERSION="${COMMONLIBSSE_VERSION}")

# ─── Plugin target ────────────────────────────────────────────────────────────
# add_commonlibsse_plugin auto-generates a translation unit holding the
# SKSEPlugin_Version block from these args. Don't define SKSEPlugin_Version
# manually in main.cpp — that produces LNK2005 ("already defined").
add_commonlibsse_plugin(${PROJECT_NAME}
    NAME    "PrismaUIAddItemMenu"
    AUTHOR  "Wondernuttz"
    VERSION 0.1.0
    USE_ADDRESS_LIBRARY     # implies VersionIndependence::AddressLibrary
    SOURCES
        src/main.cpp
        src/Settings.cpp
        src/ItemEnumerator.cpp
        src/PrismaBridge.cpp
        src/HotkeyManager.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
endif()

# Optional: copy DLL output into the mod folder after build.
# Configure with -DPIM_DEPLOY=ON to enable.
if (PIM_DEPLOY)
    set(_dest "${CMAKE_SOURCE_DIR}/mod/Data/SKSE/Plugins")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>" "${_dest}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Deploying ${PROJECT_NAME}.dll → mod/Data/SKSE/Plugins"
        VERBATIM)
endif()
