# YabauseVita

YabauseVita es un port 100% nativo del emulador de Sega Saturn **Yabause** para la PS Vita.

A diferencia de otras soluciones basadas en RetroArch o Adrenaline (emulador de PSP), YabauseVita se ejecuta directamente sobre el hardware nativo de la PS Vita. Esto nos permite acceder a toda la potencia de la consola, su RAM completa y capacidades del sistema de forma directa.

## Características

- Port nativo en C usando VitaSDK.
- Integración de interfaz directa (Vita Menu).
- **[NUEVO]** Motor JIT ARM Dinámico con soporte para ejecución en RAM (Memoria RWX).
- **[NUEVO]** Recompilador Dinámico `SH2DynARM` (ID 4) capaz de traducir instrucciones SH-2 (ALU, MOV, etc.) a código ARM nativo, acelerando el procesamiento matemático.
- **[NUEVO]** Traducción Nativa de Saltos y Delay Slots: Los saltos `BRA` y `BSR` de la Sega Saturn se compilan de forma transparente y sin interrumpir el bloque del JIT, evitando la penalización de rendimiento del fallback a C (El "Santo Grial" del emulador).
- **[NUEVO]** Arquitectura SMP (Symmetric Multi-Processing): El Master SH2 y el Slave SH2 corren de manera paralela y asíncrona distribuidos en los múltiples núcleos de hardware de la PS Vita mediante hilos nativos (`sceKernelCreateThread`), duplicando el rendimiento de procesamiento bruto de la consola.

## Instalación

1. Dirígete a la pestaña de **Releases** en GitHub.
2. Descarga el archivo `.zip` que contiene el archivo `.vpk` autogenerado.
3. Descomprime e instala el `YabauseVita.vpk` usando VitaShell.

## Compilación Local

Asegúrate de tener instalado [VitaSDK](https://vitasdk.org/).
```bash
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```
