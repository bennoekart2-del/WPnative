cmake_minimum_required(VERSION 3.20)
project(wiiparty_native C)

set(CMAKE_C_STANDARD 11)
include(FetchContent)

# Folder of C files produced by recomp/recompile.py from YOUR disc. Leave empty for a --selftest build.
set(WPR_GEN_DIR "" CACHE PATH "Generated recompiled sources (recomp_*.c, recomp_table.c)")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3  # a release archive, so building needs no git
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.24/SDL3-3.2.24.tar.gz
    URL_HASH SHA256=81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76)
FetchContent_MakeAvailable(SDL3)

# ENet (MIT, third_party/enet): reliable messages over UDP, so two home routers can be punched through
file(GLOB ENET_SRC third_party/enet/*.c)
add_library(enet STATIC ${ENET_SRC})
target_include_directories(enet PUBLIC third_party/enet/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
    target_compile_definitions(enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
        HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()

set(RUNTIME_SRC
    runtime/src/main.c runtime/src/boot.c runtime/src/cpu_services.c runtime/src/hw.c runtime/src/dsp.c
    runtime/src/gx_fifo.c runtime/src/gx_render.c runtime/src/gx_gl.c runtime/src/platform_sdl.c runtime/src/platform_audio.c runtime/src/threads.c
    runtime/src/ipc.c runtime/src/ios_devices.c runtime/src/hle_input.c runtime/src/rel_link.c runtime/src/nand_defaults.c
    runtime/src/det.c runtime/src/netplay.c)

if(WPR_GEN_DIR)
    file(GLOB GEN_SRC ${WPR_GEN_DIR}/*.c)
    set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/bigobj>")
    # The minigames (rel_*.c) are most of the code but only one runs at a time: a lighter optimization
    # level compiles them much faster and they still run at full speed.
    set(WPR_REL_OPT "-O1" CACHE STRING "Optimization flags for the minigame modules (rel_*.c)")
    file(GLOB GEN_REL_SRC ${WPR_GEN_DIR}/rel_*.c)
    if(NOT MSVC AND WPR_REL_OPT)
        set_source_files_properties(${GEN_REL_SRC} PROPERTIES COMPILE_OPTIONS "${WPR_REL_OPT}")
    endif()
else()
    set(GEN_SRC runtime/src/no_game.c)
endif()

add_executable(wiiparty-native ${RUNTIME_SRC} ${GEN_SRC})
target_include_directories(wiiparty-native PRIVATE runtime/include ${WPR_GEN_DIR})
target_link_libraries(wiiparty-native PRIVATE SDL3::SDL3-static enet)
if(WIN32)
    target_link_libraries(wiiparty-native PRIVATE ws2_32)
endif()
if(NOT MSVC)
    target_link_libraries(wiiparty-native PRIVATE m)
    target_compile_options(wiiparty-native PRIVATE -Wno-unused-label)
endif()
