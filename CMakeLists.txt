cmake_minimum_required(VERSION 3.20)
project(wiiparty_native C)

set(CMAKE_C_STANDARD 11)
include(FetchContent)

# Folder of C files produced by recomp/recompile.py from YOUR disc. Leave empty for a --selftest build.
set(WPR_GEN_DIR "" CACHE PATH "Generated recompiled sources (recomp_*.c, recomp_table.c)")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.24 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

set(RUNTIME_SRC
    runtime/src/main.c runtime/src/boot.c runtime/src/cpu_services.c runtime/src/hw.c runtime/src/dsp.c
    runtime/src/gx_fifo.c runtime/src/gx_render.c runtime/src/platform_sdl.c runtime/src/threads.c
    runtime/src/ipc.c runtime/src/ios_devices.c runtime/src/hle_input.c runtime/src/rel_link.c runtime/src/nand_defaults.c)

if(WPR_GEN_DIR)
    file(GLOB GEN_SRC ${WPR_GEN_DIR}/*.c)
    set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/bigobj>")
else()
    set(GEN_SRC runtime/src/no_game.c)
endif()

add_executable(wiiparty-native ${RUNTIME_SRC} ${GEN_SRC})
target_include_directories(wiiparty-native PRIVATE runtime/include ${WPR_GEN_DIR})
target_link_libraries(wiiparty-native PRIVATE SDL3::SDL3-static)
if(NOT MSVC)
    target_link_libraries(wiiparty-native PRIVATE m)
    target_compile_options(wiiparty-native PRIVATE -Wno-unused-label)
endif()
