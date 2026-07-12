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
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -flto -O3 -funroll-loops -fgcse-after-reload -fweb -frename-registers")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "YabauseVita")
set(VITA_TITLEID  "YABA00001")
set(VITA_VERSION  "01.00")

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
    src/vita/vita_menu.c
    src/vita/sh2fast.c
    src/vita/sh2lru.c
    src/vita/vidgpu.c
)

add_executable(${VITA_APP_NAME}
    ${YABAUSE_CORE_SOURCES}
    ${VITA_PLATFORM_SOURCES}
)

target_include_directories(${VITA_APP_NAME} PRIVATE src)

# Disable LTO for main.c to prevent aggressive optimizer reordering
# of code before unbounded while loops
set_source_files_properties(src/vita/main.c PROPERTIES COMPILE_FLAGS "-fno-lto")

# Extra optimization for SH2 cores and video renderer
set_source_files_properties(src/vita/sh2lru.c PROPERTIES COMPILE_FLAGS "-O3 -funroll-loops -fgcse-after-reload")
set_source_files_properties(src/vita/sh2fast.c PROPERTIES COMPILE_FLAGS "-O3 -funroll-loops")
set_source_files_properties(src/sh2int.c PROPERTIES COMPILE_FLAGS "-O3 -funroll-loops")
set_source_files_properties(src/vidsoft.c PROPERTIES COMPILE_FLAGS "-O3 -funroll-loops -fgcse-after-reload")

target_link_libraries(${VITA_APP_NAME}
    vita2d
    SceDisplay_stub
    SceCtrl_stub
    SceAudio_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceGxm_stub
    SceIofilemgr_stub
    m
)

set(VITA_MAKE_FSELF_FLAGS "${VITA_MAKE_FSELF_FLAGS} -a 0x3800000000000001")

vita_create_self(${VITA_APP_NAME}.self ${VITA_APP_NAME}
    UNSAFE
    NOASLR
    UNCOMPRESSED
)

vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} ${VITA_APP_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png                              sce_sys/icon0.png
    FILE sce_sys/bg0.png                                sce_sys/bg0.png
    FILE sce_sys/startup.png                            sce_sys/startup.png
    FILE sce_sys/livearea/contents/template.xml         sce_sys/livearea/contents/template.xml
)
