cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(YabauseVita C)
add_definitions(-DVERSION="0.9.10")
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "YabauseVita")
set(VITA_TITLEID  "YABA00001")
set(VITA_VERSION  "00.10")

# -----------------------------------------------------------------------
# MILESTONE 0 source list.
# This is the "libyabause_a_SOURCES" list from the original Makefile.am,
# with the platform-specific pieces removed:
#   - NO c68k/ or q68/       (not needed: we use M68KCORE_DUMMY)
#   - NO vidogl.c / ygl.c    (not needed: we use VIDCORE_SOFT, and ygl.c
#                              pulls in SDL headers we don't want yet)
#   - NO sndal.c / sndsdl.c  (not needed: we use SNDCORE_DUMMY)
#   - NO persdljoy.c         (not needed: we use PERCORE_DUMMY)
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

set(VITA_PLATFORM_SOURCES
    src/vita/main.c
)

add_executable(${VITA_APP_NAME}
    ${YABAUSE_CORE_SOURCES}
    ${VITA_PLATFORM_SOURCES}
)

target_include_directories(${VITA_APP_NAME} PRIVATE src)

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

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${PROJECT_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
