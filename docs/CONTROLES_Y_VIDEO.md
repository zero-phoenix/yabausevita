# Controles, video y overclock (v01.02)

## Controles

### El bug de START (arreglado)

Hasta v01.01, el loop de emulación tenía esto:

```c
if (cur & SCE_CTRL_START)
    break;              /* → sceKernelExitProcess(0) */
```

Cualquier presión de START **cerraba la app al LiveArea** — por eso en
Sonic R no se podía pausar. Ahora START llega al juego como el botón
Start del Saturn, y para salir hay que **mantener START+SELECT ~1
segundo**.

### Mapeo por defecto (v2)

Posicional — el estándar que usa RetroArch para Saturn en mandos de
4 botones frontales:

```
Saturn:  X  Y  Z            Vita:       (△ = Y)
         A  B  C                  (□ = A)     (○ = C)
                                        (✕ = B)
```

| Vita          | Saturn |
|---------------|--------|
| Cruceta       | Cruceta |
| Analógico izq.| Cruceta (zona muerta ±50) |
| □             | A      |
| ✕             | B      |
| ○             | C      |
| △             | Y      |
| L / R         | L / R  |
| SELECT        | X      |
| START         | Start  |
| START+SELECT (1 s) | Salir al LiveArea |

Z queda asignable desde el menú de controles. El mapeo del menú ahora
**sí se respeta**: antes `main()` forzaba `set_default_mapping()` al
arrancar cada juego y pisaba lo configurado. Los configs guardados por
versiones anteriores se migran una sola vez al nuevo mapeo mediante la
clave `map_version=2`.

## Video

### Colores (tonalidades raras — arreglado)

vidsoft empaqueta cada píxel como `0xAABBGGRR` (R en el byte bajo; ver
`COLSAT2YAB32` y `Vdp2ColorRamGetColor`), que en memoria little-endian
es la secuencia de bytes `R,G,B,A` = formato GXM `A8B8G8R8`. La textura
se creaba como `A8R8G8B8`, así que la GPU **intercambiaba rojo y azul**.
Un solo cambio de formato lo corrige, sin costo por píxel.

### Escalado (imagen pequeña — arreglado)

Antes el frame se dibujaba 1:1 centrado (320×224 en una pantalla de
960×544 ≈ un tercio). Ahora:

- **Altura máxima**: escala = `544 / alto_del_juego` (p. ej. 320×224 →
  ×2.43 → 777×544).
- **Proporcional, sin estirar**: la misma escala en X e Y.
- **Centrado** horizontal; toda resolución del Saturn cabe a lo ancho.
- **Sin filtros**: la textura usa `SCE_GXM_TEXTURE_FILTER_POINT`
  (vecino más cercano) — píxeles nítidos, cero blur.

También se eliminó la ruta vieja de video de `main.c` (memcpy sin
escalar + `sceDisplaySetFrameBuf`), que competía con los buffers de
vita2d y podía parpadear. Libera además los 2 MB de su framebuffer.

## Overclock y PSVshell

Al arrancar, la app fija los relojes al máximo del API oficial:

```c
scePowerSetArmClockFrequency(444);   /* CPU  */
scePowerSetBusClockFrequency(222);   /* BUS  */
scePowerSetGpuClockFrequency(222);   /* GPU  */
scePowerSetGpuXbarClockFrequency(166);
```

Se fijan **una sola vez** y nunca se vuelven a tocar durante la
emulación, así **PSVshell** puede subirlos (p. ej. ARM a 500 MHz con
el plugin instalado) o bajarlos sin que la app se los pise. Los valores
reales quedan registrados en `ux0:data/yabausevita_log.txt` al inicio
(`Clocks: ARM=... BUS=... GPU=... XBAR=...`).

Requiere `ScePower_stub` (agregado al CMakeLists).
