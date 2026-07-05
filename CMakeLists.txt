cmake_minimum_required(VERSION 3.10)
project(YabauseVita C)

# 1. INCLUSIÓN CRÍTICA DE VITASDK
if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK environment variable not set.")
endif()
include("$ENV{VITASDK}/share/vita.cmake")

set(CMAKE_C_STANDARD 11)

# 2. BUSCAR TODOS LOS ARCHIVOS FUENTE AUTOMÁTICAMENTE
# Esto escanea tu repositorio y encuentra todos los .c y .h sin que tengas que nombrarlos uno por uno
file(GLOB_RECURSE YABAUSE_SRC
    "*.c"
    "*.h"
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

# 6. INCLUIR DIRECTORIOS DE CABECERAS
# Esto asegura que el compilador encuentre los archivos .h en las subcarpetas
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
# Usamos un nombre distinto para el target del VPK y damos permisos UNSAFE al SELF
vita_create_self(yabause_vita.self yabause_vita UNSAFE)

# Generamos el VPK. Si no tienes param.sfo o icon0.png, CMake usará los valores por defecto.
vita_create_vpk(YabauseVita.vpk "YABAUSE00" yabause_vita.self
    NAME "Yabause Vita"
)
