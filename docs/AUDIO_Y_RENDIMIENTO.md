# Audio y rendimiento (v01.03)

## Audio original del juego

Hasta v01.02 el emulador corría con `SNDCORE_DUMMY` y el 68K en dummy:
**silencio total**. Ahora:

- **`src/vita/snd_vita.c`** — backend de sonido nativo con `sceAudioOut`
  (puerto BGM, 44100 Hz estéreo, búfer de 512 muestras ≈ 11.6 ms).
  Doble búfer con handshake productor/consumidor, el mismo patrón probado
  del port de PSP (`psp-sound.c`): el puerto de audio hace de reloj, sin
  busy-wait.
- **68K real (Q68)** — el core portable de Andrew Church que ya estaba en
  el árbol (`src/q68/`), en modo intérprete puro en ARM. Ejecuta el
  driver de sonido que cada juego sube al CPU de sonido del Saturn:
  **efectos de sonido y música secuenciada** funcionan.
- **CDDA** — la música por pistas de audio del CD (como la banda sonora
  de Sonic R) fluye desde el CD block (`cs2.c → ScspReceiveCDDA`) y el
  lector CHD ya entrega los sectores de audio con el byte-swap correcto.
- El interruptor **"Audio habilitado"** y el **volumen** del menú ahora
  hacen efecto de verdad. Con audio OFF, el 68K y el SCSP quedan en dummy
  y ese tiempo de CPU se recupera.

La misma combinación (Q68 + backend de doble búfer) es la que usaba el
port oficial de PSP de Yabause — hardware más débil que la Vita.

## Auto-frameskip real (más fps)

El "frameskip" anterior **no saltaba nada**: ambas ramas del loop
llamaban a `YabauseExec()` igual y vidsoft renderizaba todas las capas,
componía y presentaba cada frame de todos modos.

Ahora la decisión se toma **al inicio de cada frame** (`Vdp2DrawStart`
en `vidgpu.c`) contra un deadline de reloj real (16.67 ms NTSC / 20 ms
PAL):

- **Al día** → se renderiza y presenta; el vsync de vita2d marca el paso
  a 60 fps y de paso impide que el audio se adelante.
- **Retrasado más de medio frame** → se salta lo caro: render de capas
  VDP2 (`VIDSoftVdp2DrawScreens`), composición, subida a GPU, present y
  espera de vsync. Máximo 4 saltos consecutivos.
- El **VDP1 nunca se salta** (hay juegos que leen su framebuffer) y el
  **audio nunca se salta** (SCSP corre por línea): la música sigue fluida
  aunque el video salte frames.
- Tras pausas largas (cargas de CD) el deadline se resincroniza solo.

En el menú: `auto_frameskip` (por defecto ON) usa este modo; si lo
apagas, `frame_skip` N fija el patrón clásico "renderiza 1 de N+1".
El log de 5 s ahora incluye `skipped=` para ver cuántos frames saltó.

## Interacción con el overclock (PSVshell)

Los clocks se fijan una vez al arrancar (444/222/222/166) y PSVshell
puede subirlos a 500 MHz. Con más CPU:

- el deadline se cumple más a menudo → **menos frames saltados**,
- el Q68 + SCSP (audio) cuestan un % menor del frame,
- juegos que antes iban al límite se quedan en 60 fps estables.

Costo estimado del audio activado: el 68K a 11.3 MHz emulado por
intérprete + mezcla SCSP. Si un juego pesado lo nota, apagar audio en el
menú lo recupera.
