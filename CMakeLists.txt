cmake_minimum_required(VERSION 3.10)
project(YabauseVita C)

# 1. INCLUSIÓN CRÍTICA DE VITASDK
if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK environment variable not set.")
endif()
include("$ENV{VITASDK}/share/vita.cmake")

set(CMAKE_C_STANDARD 11)

# 2. BUSCAR TODOS LOS ARCHIVOS FUENTE AUTOMÁTICAMENTE
file(GLOB_RECURSE YABAUSE_SRC
    "*.c"
    "*.h"
)

# 3. CREACIÓN DEL EJECUTABLE
add_executable(yabause_vita ${YABAUSE_SRC})

# 4. OPTIMIZACIONES BÁSICAS
# Quitamos -mfpu=neon y -mcpu=cortex-a9 porque VitaSDK ya lo hace por defecto
target_compile_options(yabause_vita PRIVATE -O3 -ftree-vectorize -fno-exceptions)

# 5. DEFINICIONES PARA YABAUSE (Rendimiento Nativo)
target_compile_definitions(yabause_vita PRIVATE
    DYNAREC=1
    HAVE_VITA=1
    HAVE_VITA_GL=1
    LOG_LEVEL=3
)

# 6. INCLUIR DIRECTORIOS DE CABECERAS
target_include_directories(yabause_vita PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 7. ENLACE DE LIBRERÍAS
target_link_libraries(yabause_vita
    PRIVATE
    VitaGL
    vita2d
    freetype
    m
    pthread
    SceAppMgr_stub
    SceAudio_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceSysmodule_stub
    ScePower_stub
)

# 8. GENERACIÓN DEL VPK
vita_create_self(yabause_vita.self yabause_vita UNSAFE)
vita_create_vpk(YabauseVita.vpk "YABAUSE00" yabause_vita.self
    NAME "Yabause Vita"
)
