cmake_minimum_required(VERSION 3.10)
project(YabauseVita C)

# Incluir el toolchain de VitaSDK
set(CMAKE_C_STANDARD 11)

# Buscar paquetes necesarios de VitaSDK
find_package(VitaGL REQUIRED)
find_package(vita2d REQUIRED)
find_package(FT2 REQUIRED)

# Definir los archivos fuente de Yabause (ajusta esto a tu estructura)
set(YABAUSE_SRC
    src/main.c
    src/vita/input_vita.c
    src/vita/video_vita.c
    src/vita/audio_vita.c
    # ... (añade el resto del core de Yabause: sh2core, smpc, vdp1, vdp2, etc.)
)

# Añadir ejecutable
add_executable(yabause_vita ${YABAUSE_SRC})

# Optimizaciones para el CPU Cortex-A9 de PS Vita y activación de NEON
target_compile_options(yabause_vita PRIVATE -O3 -mfpu=neon -mcpu=cortex-a9 -ftree-vectorize -fno-exceptions)

# Definiciones de compilación para Yabause en Vita
target_compile_definitions(yabause_vita PRIVATE
    DYNAREC=1          # Vital para rendimiento de SH-2
    HAVE_VITA=1
    HAVE_VITA_GL=1     # Usar GPU para renderizado
    LOG_LEVEL=3
)

# Enlazar librerías del sistema y de Vita
target_link_libraries(yabause_vita
    PRIVATE
    VitaGL::VitaGL
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

# --- SOLUCIÓN AL ERROR DE GITHUB ACTIONS ---
# 1. Crear el SELF con permisos UNSAFE (para leer ISOs)
vita_create_self(yabause_vita.self yabause_vita UNSAFE)

# 2. Crear el VPK sin colisión de nombres
# Asegúrate de que el Title ID (YABAUSE00) sea único
vita_create_vpk(YabauseVita.vpk "YABAUSE00" yabause_vita.self
    NAME "Yabause Vita"
    FILE resources/param.sfo
    FILE resources/icon0.png
)
