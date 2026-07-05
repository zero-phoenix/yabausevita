cmake_minimum_required(VERSION 3.10)
project(YabauseVita C)

# 1. INCLUSIÓN CRÍTICA DE VITASDK
# Esto habilita las macros vita_create_self y vita_create_vpk
if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK environment variable not set.")
endif()
include("$ENV{VITASDK}/share/vita.cmake")

set(CMAKE_C_STANDARD 11)

# 2. DEFINICIÓN DE ARCHIVOS FUENTE
# Asegúrate de listar aquí todos los archivos .c del core de Yabause y la carpeta vita
set(YABAUSE_SRC
    src/main.c
    src/vita/input_vita.c
    src/vita/video_vita.c
    src/vita/audio_vita.c
    # ... (añade todos los archivos necesarios)
)

# 3. CREACIÓN DEL EJECUTABLE
add_executable(yabause_vita ${YABAUSE_SRC})

# 4. OPTIMIZACIONES PARA PS VITA (CPU Cortex-A9 + NEON)
target_compile_options(yabause_vita PRIVATE -O3 -mfpu=neon -mcpu=cortex-a9 -ftree-vectorize -fno-exceptions)

# 5. DEFINICIONES PARA YABAUSE (Rendimiento Nativo)
target_compile_definitions(yabause_vita PRIVATE
    DYNAREC=1          # Vital para la emulación del SH-2 a buena velocidad
    HAVE_VITA=1
    HAVE_VITA_GL=1     # Usa la GPU de Vita via vitaGL
    LOG_LEVEL=3
)

# 6. ENLACE DE LIBRERÍAS (Sin find_package, enlazando directamente)
target_link_libraries(yabause_vita
    PRIVATE
    VitaGL          # Se enlaza directamente por nombre
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

# 7. GENERACIÓN DEL VPK (Sin errores de colisión de targets)
# Usamos un nombre distinto para el target del VPK y damos permisos UNSAFE al SELF
vita_create_self(yabause_vita.self yabause_vita UNSAFE)
vita_create_vpk(YabauseVita.vpk "YABAUSE00" yabause_vita.self
    NAME "Yabause Vita"
    FILE resources/param.sfo
    FILE resources/icon0.png
)
