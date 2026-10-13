# nhl3on3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nhl3on3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Windows clang presets leave the per-config flags empty, which builds the
# recompiled guest code at -O0.
add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-O2>)

include(generated/rexglue.cmake)

# Sources
set(NHL3ON3_SOURCES
    src/main.cpp
    src/xam_input_lock.cpp
    src/xusbcam_stubs.cpp
    src/nhl3on3_display.cpp
    src/nhl3on3_net.cpp
    src/nhl3on3_guest_log.cpp
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
