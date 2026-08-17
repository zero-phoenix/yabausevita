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

# ── Flags críticos de rendimiento (v01.05) ─────────────────────────────
# DONT_PROFILE: sin él, PROFILE_START/STOP son funciones REALES (clock() +
#   búsqueda de strings) llamadas ~2.5 MILLONES de veces/segundo desde el
#   loop de emulación — la causa principal de que el emulador nunca pasara
#   de unos pocos fps en ninguna versión anterior.
# NO_DECILINE: rebanadas SH2 por línea (~1700 ciclos) en vez de por
#   deciline (~170): 10× menos llamadas a SH2Exec/ScuExec. Lo mismo que
#   usaba el port oficial de PSP.
# PSP_TIMING_TWEAKS: contabilidad de ciclos más barata (FRT por ciclos
#   pasados en vez de acumulados), también del port de PSP.
# HAVE_GETTIMEOFDAY: sin él, YabauseGetTicks() no tiene rama válida en
#   Vita (retorna basura) y tickfreq queda en 0 — el auto-frameskip
#   interno calculaba sobre basura.
# HAVE_SYS_TIME_H: yabause.c guarda el include de <sys/time.h> con esta
#   macro (no con HAVE_GETTIMEOFDAY); sin ella gettimeofday queda sin
#   declarar y GCC 15 lo trata como error.
add_definitions(-DDONT_PROFILE -DNO_DECILINE -DPSP_TIMING_TWEAKS -DHAVE_GETTIMEOFDAY -DHAVE_SYS_TIME_H)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Ofast -flto -ftree-vectorize -funroll-loops -fgcse-after-reload -fweb -frename-registers -mfpu=neon -ffast-math -fno-math-errno -fno-strict-aliasing")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "YabauseVita")
set(VITA_TITLEID  "YABA00001")
set(VITA_VERSION  "01.05")

set(YABAUSE_CORE_SOURCES
    src/bios.c
    src/junzip.c
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
    src/m68kq68.c
    src/q68/q68.c
    src/q68/q68-core.c
    src/q68/q68-disasm.c
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
    src/vita/cd_chd.c
    src/vita/libchdr/unity.c
    src/vita/sh2fast.c
    src/vita/sh2lru.c
    src/vita/snd_vita.c
    src/vita/vidgpu.c
    src/vita/vidvitagl.c
    src/vita/sh2dyn_arm.c
)

add_executable(${VITA_APP_NAME}
    ${YABAUSE_CORE_SOURCES}
    ${VITA_PLATFORM_SOURCES}
)

target_include_directories(${VITA_APP_NAME} PRIVATE
    src
    src/vita/libchdr/include
    src/vita/libchdr/deps/lzma-25.01/include
    src/vita/libchdr/deps/miniz-3.1.1
    src/vita/libchdr/deps/zstd-1.5.7
)

# libchdr (lector CHD): un solo TU; -O2 basta y compila más rápido
set_source_files_properties(src/vita/libchdr/unity.c PROPERTIES COMPILE_FLAGS "-O2")

# Disable LTO for main.c to prevent aggressive optimizer reordering
# of code before unbounded while loops
set_source_files_properties(src/vita/main.c PROPERTIES COMPILE_FLAGS "-fno-lto")

# Extra optimization for SH2 cores and video renderer
set_source_files_properties(src/vita/sh2lru.c PROPERTIES COMPILE_FLAGS "-Ofast -funroll-loops -fgcse-after-reload")
set_source_files_properties(src/vita/sh2fast.c PROPERTIES COMPILE_FLAGS "-Ofast -funroll-loops")
set_source_files_properties(src/sh2int.c PROPERTIES COMPILE_FLAGS "-Ofast -funroll-loops")
set_source_files_properties(src/vidsoft.c PROPERTIES COMPILE_FLAGS "-Ofast -funroll-loops -fgcse-after-reload")
# Q68 (CPU de sonido 68000) y SCSP: intérpretes calientes con audio activo
set_source_files_properties(src/q68/q68-core.c PROPERTIES COMPILE_FLAGS "-Ofast -funroll-loops")
set_source_files_properties(src/scsp.c PROPERTIES COMPILE_FLAGS "-Ofast")

target_link_libraries(${VITA_APP_NAME}
    vita2d
    SceDisplay_stub
    SceCtrl_stub
    SceAudio_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceGxm_stub
    SceIofilemgr_stub
    ScePower_stub
    m
    z
)

vita_create_self(${VITA_APP_NAME}.self ${VITA_APP_NAME})

vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} ${VITA_APP_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

