cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(YabauseVita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "YabauseVita")
set(VITA_TITLEID  "YABA00001")
set(VITA_VERSION  "00.20")

# -----------------------------------------------------------------------
# Portable Yabause core sources (no PSP/MIPS-specific code, no platform
# GL/SDL dependencies -- see PORTING_NOTES.md for the full reasoning).
# -----------------------------------------------------------------------
set(YABAUSE_CORE_SOURCES
    src/bios.c
    src/cdbase.c
    src/cheat.c
    src/coffelf.c
    src/cs0.c
    src/cs1.c
    src/cs2.c
    src/debug.c
    src/error.c
    src/memory.c
    src/m68kcore.c
    src/m68kd.c
    src/movie.c
    src/netlink.c
    src/peripheral.c
    src/profile.c
    src/scu.c
    src/sh2core.c
    src/sh2d.c
    src/sh2idle.c
    src/sh2int.c
    src/smpc.c
    src/scsp.c
    src/vdp1.c
    src/vdp2.c
    src/vdp2debug.c
    src/vidshared.c
    src/vidsoft.c
    src/yabause.c
)

# -----------------------------------------------------------------------
# debugScreen.c/.h come from the REAL vitasdk/samples repo (cloned by
# the CI workflow into external/vitasdk-samples/common/) instead of
# being hand-typed here -- it's a fairly intricate font/ANSI-sequence
# implementation and transcribing it by hand is how bugs sneak in.
# -----------------------------------------------------------------------
set(DEBUGSCREEN_DIR "${CMAKE_SOURCE_DIR}/external/vitasdk-samples/common")

set(VITA_PLATFORM_SOURCES
    src/vita/main.c
    ${DEBUGSCREEN_DIR}/debugScreen.c
)

add_executable(${VITA_APP_NAME}
    ${YABAUSE_CORE_SOURCES}
    ${VITA_PLATFORM_SOURCES}
)

target_include_directories(${VITA_APP_NAME} PRIVATE
    src
    ${DEBUGSCREEN_DIR}
)

target_link_libraries(${VITA_APP_NAME}
    SceDisplay_stub
    SceCtrl_stub
    SceAudio_stub
    SceSysmodule_stub
    SceGxm_stub
    SceCommonDialog_stub
    SceAppMgr_stub
    SceIofilemgr_stub
    m
)

vita_create_self(${VITA_APP_NAME}.self ${VITA_APP_NAME})
vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} ${VITA_APP_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
