# YabauseVita

Port nativo en C del emulador de Sega Saturn **Yabause** para la PS Vita,
con interfaz propia (menú Vita), lectura directa de CHD, audio real en hilo
dedicado y un ciclo de optimización medido y documentado.

> **Fuente de verdad:** `docs/BITACORA-OPTIMIZACION.md`. Este README describe
> el estado verificado; la bitácora explica cómo se midió cada afirmación.
> `PORTING_NOTES.md` quedó desactualizado en el hito 0 — no usarlo como
> referencia del estado real (hallazgo A6).

## Estado real (verificado con capturas, 30-ago-2026)

| Juego | FPS en Vita3K | Imagen | Movimiento |
|---|---|---|---|
| Panzer Dragoon (EU) | **59,8 / 60** | ✅ | ✅ 45,9 % de píxeles cambiando |
| Sonic R (EU) | 46,3 | ✅ | ✅ 5,0 % |
| NiGHTS (USA) | 40,0 | parcial | arranque lento (usa los dos SH2 a tope) |

Medido con el banco automatizado (`tools/vita3k_ctl.py`): capturas continuas
de la ventana del juego con veredicto de imagen y movimiento — un FPS solo
no es evidencia (regla R9 de la bitácora).

## Características

- **Núcleos de emulación**: intérprete rápido (SH2Fast) y con caché LRU
  (SH2LRU), ambos verificados en runtime. Un dynarec ARM (`SH2DynARM`) existe
  pero **cuelga al primer frame** — ver «Pendiente».
- **Vídeo**: rasterizado por software (VDP1+VDP2) con presentación por GPU
  (vita2d), hilo de render dedicado y auto-frameskip interno.
- **Audio**: backend sceAudioOut + 68K Q68 ejecutando el driver de sonido del
  juego, en hilo dedicado.
- **CD**: lectura directa de CHD (libchdr), sin extracción a `.bin`.
- **Menú propio**: pestañas ROMs/BIOS/CONFIG, mapeo de botones configurable,
  emparejamiento de BIOS por región, `autostart` para rondas automatizadas.
- **Instrumentación**: cada 5 s el log trae FPS del ROM, desglose GPU
  (drawn/presented/dropped/composite/upload/display) y desglose EMU por
  subsistema (msh2/ssh2/scu/scsp/scsp_th/m68k/hblank/vdp/cdb/smpc).
  Con `show_fps=1`, el FPS del ROM se dibuja sobre el juego.

## Instalación

1. Pestaña **Releases** → descarga el `.zip` con el `YabauseVita.vpk`.
2. Instálalo con VitaShell (Title ID `YABA00001`).
3. Copia tus juegos (`.chd`) a `ux0:data/yabause/roms/` y una BIOS real a
   `ux0:data/yabause/bios/{jp,us,eu}/` — la región de la BIOS debe casar con
   la del disco o la Saturn no enciende el vídeo (hallazgo A15).

## Compilación

Con [VitaSDK](https://vitasdk.org/):

```bash
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Flags: `-O3` plano. Los `-Ofast -flto -ffast-math` se probaron y se quitaron
(commit a674198 revertido en la ronda 1): un emulador que compara bits no se
compila con mates rápidas.

La CI de GitHub Actions compila cada push a `main` y publica el VPK como
artefacto y release automático.

## Herramientas del ciclo

- `tools/vita3k_ctl.py` — lanza Vita3K sin elevar (y sin el OpenSSL de Git en
  el PATH), arranca el emulador por sí solo, toma capturas continuas de la
  ventana del juego con veredicto de imagen/movimiento, y edita el config sin
  BOM (con BOM, `rom_path` se ignora en silencio — A16).
- `docs/BITACORA-OPTIMIZACION.md` — cada ronda deja lo medido y las reglas
  derivadas. Nada se borra.
- `docs/MEGAPLAN-R1-INSTRUMENTACION.md` — el plan de la ronda 1, ejecutado.

## Las tres filosofías

Las rondas de optimización proponen por tres caminos ortogonales, cada uno
con su métrica y su predicción falsable (bitácora §2):

| | Filosofía | Ataca | Riesgo característico |
|---|---|---|---|
| A | **Hacer menos** — el píxel más rápido es el que no se dibuja | `composite` | invalidación incompleta: fantasmas a los 3 minutos |
| B | **Mover menos** — el bus es un recurso como el reloj | `upload` | carreras rasterizador/GPU al compartir memoria |
| C | **Repartir mejor** — tres núcleos y el juego usa uno y medio | `dropped` | el reparto que va bien en un juego va mal en otro |

Son ortogonales por construcción: si las tres apuntaran al mismo mecanismo,
la comparación no distinguiría nada.

## Pendiente (con evidencia, no con esperanzas)

- **Dynarec `SH2DynARM` cuelga al primer frame** con cache JIT correcta
  (8 MB RWX); el hang está en la ejecución del código generado. No probado
  en hardware real — Vita3K decide corrección, no rendimiento (R4).
- NiGHTS no llegó al título en 75 s (arranque largo a 40 FPS).
- Verificar que el input cruza al juego en corrida automatizada.
- El blanco de la Ronda 2/3 ya tiene nombre: **los SH2** — Sonic R y NiGHTS
  gastan 70-86 % del tiempo de emulación en msh2+ssh2.

## Créditos

- [Yabause](https://yabause.org) — el emulador base.
- [Kronos](https://github.com/FCare/Kronos) — de donde venían (y de donde se
  sacaron, por incompatibles con este rasterizador) los núcleos VDP.
- [libchdr](https://github.com/rtissera/libchdr) — lectura de CHD.
