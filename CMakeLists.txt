# nhl3on3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nhl3on3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Windows clang presets leave the per-config flags empty, which builds the
# recompiled guest code at -O0.
add_compile_options($<$<CONFIG:RelWithDebInfo>:-O2>)

# Release is the shipping build: ThinLTO over the recompiled code and a portable
# x86-64-v3 (AVX2) target. Never -march=native: on AMD it enables SSE4a, whose
# EXTRQ/INSERTQ crash on Intel CPUs.
set(NHL3ON3_RELEASE_ARCH "x86-64-v3" CACHE STRING "-march for Release builds")
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Release>:-march=${NHL3ON3_RELEASE_ARCH}>
    $<$<CONFIG:Release>:-flto=thin>)
add_link_options($<$<CONFIG:Release>:-flto=thin>)

include(generated/rexglue.cmake)

# Sources
set(NHL3ON3_SOURCES
    src/main.cpp
    src/xam_input_lock.cpp
    src/xusbcam_stubs.cpp
    src/nhl3on3_display.cpp
    src/nhl3on3_widescreen.cpp
    src/nhl3on3_net.cpp
    src/nhl3on3_guest_log.cpp
    src/nhl3on3_remote_pad.cpp
    src/nhl3on3_dist_trace.cpp
    src/nhl3on3_lockstep_relay.cpp
    src/nhl3on3_ui.cpp
    src/nhl3on3_ui_patch.cpp
    src/nhl3on3_stfs.cpp
    src/nhl3on3_save_trace.cpp
    src/nhl3on3_settings.cpp
    src/nhl3on3_team_lobby.cpp
)

if(WIN32)
    add_executable(nhl3on3 WIN32 ${NHL3ON3_SOURCES})
else()
    add_executable(nhl3on3 ${NHL3ON3_SOURCES})
endif()

rexglue_setup_target(nhl3on3)

if(WIN32)
    target_compile_definitions(nhl3on3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(nhl3on3 PRIVATE ws2_32)
endif()

# Stage the SDK's shared runtime and the Xenos GPU plugin beside the executable.
get_filename_component(NHL3ON3_SDK_ROOT "${rexglue_DIR}/../../.." ABSOLUTE)
if(WIN32)
    file(GLOB NHL3ON3_SDK_SHARED "${NHL3ON3_SDK_ROOT}/bin/*.dll")
else()
    file(GLOB NHL3ON3_SDK_SHARED "${NHL3ON3_SDK_ROOT}/lib/*.so")
endif()
add_custom_command(TARGET nhl3on3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NHL3ON3_SDK_SHARED} $<TARGET_FILE_DIR:nhl3on3>
    VERBATIM)

# Settings launcher (writes nhl3on3.toml, which the game loads at startup):
# a PowerShell script on Windows, an SDL3 + Dear ImGui program elsewhere.
if(WIN32)
    add_custom_command(TARGET nhl3on3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher/nhl3on3_launcher.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher/NHL3on3 Launcher.bat"
            $<TARGET_FILE_DIR:nhl3on3>
        VERBATIM)
else()
    find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-static)
    set(NHL3ON3_IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/launcher/thirdparty/imgui")
    add_executable(nhl3on3-launcher
        launcher/nhl3on3_launcher.cpp
        launcher/nhl3on3_launcher_window.cpp
        ${NHL3ON3_IMGUI}/imgui.cpp
        ${NHL3ON3_IMGUI}/imgui_draw.cpp
        ${NHL3ON3_IMGUI}/imgui_tables.cpp
        ${NHL3ON3_IMGUI}/imgui_widgets.cpp
        ${NHL3ON3_IMGUI}/backends/imgui_impl_sdl3.cpp
        ${NHL3ON3_IMGUI}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(nhl3on3-launcher BEFORE PRIVATE
        ${NHL3ON3_IMGUI} ${NHL3ON3_IMGUI}/backends)
    target_compile_definitions(nhl3on3-launcher PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_link_libraries(nhl3on3-launcher PRIVATE SDL3::SDL3-static)
    set_target_properties(nhl3on3-launcher PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:nhl3on3>)
    add_dependencies(nhl3on3 nhl3on3-launcher)
endif()
