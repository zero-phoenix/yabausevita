# Audio y rendimiento (v01.03 → v01.05)

## v01.05: los 3 núcleos + las patologías del loop

Con el audio ya en su núcleo (v01.04) el juego seguía a ~1 fps: el hilo
principal era el cuello. Diagnóstico y paquete de fixes:

**1. `-DHAVE_GETTIMEOFDAY` (crítico).** En Vita, `YabauseGetTicks()` no
tenía rama válida (retornaba **basura**, sin `return`) y `tickfreq`
quedaba en 0. El auto-frameskip interno de vdp2.c — activo desde
siempre vía la config — decidía saltos y límites de velocidad sobre
matemática rota, de forma no determinista. Ahora mide microsegundos
reales.

**2. `-DNO_DECILINE` + `-DPSP_TIMING_TWEAKS` (los flags del port de
PSP).** El loop ejecutaba los SH2 en rebanadas de ~170 ciclos: 5 260
llamadas a `SH2Exec`/`ScuExec` por frame (315 800/s), y el overhead por
llamada (chequeo de idle, setup del cache de bloques) dominaba el tiempo
total. Con rebanadas por línea son 10× menos llamadas, con HBlankIN
preservado (9/10 + 1/10 de línea).

**3. Auto-frameskip interno de vdp2.c como único governor**
(`yinit.frameskip = 1` siempre). Mide ticks reales (ya correctos por el
punto 1), salta frames completos cuando va retrasado — **incluido el
render del VDP1**, con `Vdp1NoDraw` manteniendo la semántica de EDSR que
los juegos esperan — hasta 9 seguidos, y limita la velocidad cuando el
juego va sobrado. Se eliminó el frameskip por deadline de vidgpu (dos
sistemas de skip peleándose).

**4. Hilo de render en el núcleo 1.** De los frames que sí se dibujan,
la mitad cara de la salida — Composite (mezcla VDP1+VDP2 por píxel),
subida a GPU, draw y **la espera de vsync** — corre en un hilo fijado al
núcleo 1, con doble búfer del framebuffer VDP2 (ping-pong sin memcpy) y
el swap del VDP1 movido al handoff (hilo principal, solo cuando el
render está libre → sin carreras). Si el render sigue ocupado cuando
llega el siguiente frame, ese frame no se presenta y punto: el hilo
principal **jamás** espera a la GPU ni al vsync.

```
núcleo 0: SH2 + SCU + VDP1 + capas VDP2   (el juego)
núcleo 1: Composite + upload + present + vsync
núcleo 2: 68K + timers SCSP + mezcla + CDDA
```

**5. `-DDONT_PROFILE`** por higiene (yabause.c ya lo definía localmente;
ahora aplica a todo el árbol).

**Telemetría** (en `ux0:data/yabausevita_log.txt`, cada 5 s):
`FPS:` = velocidad real del juego (frames emulados), y
`GPU: drawn/presented/dropped` + µs de composite/upload/display.
Si algo sigue lento, ese log dice exactamente dónde mirar.

---

# Registro histórico v01.04

## v01.04: por qué v01.03 iba a ~0 fps, y la solución

**Diagnóstico.** En v01.03 el subsistema de sonido completo corría en el
hilo principal, y `ScspExec` (una vez por línea, 15 780 veces/segundo)
llamaba al mezclador de 32 slots (`scsp_update`) clockeado a 44 100 Hz de
**tiempo real** por `GetAudioSpace` — un impuesto fijo de CPU
independiente de la velocidad de emulación. Encima, el 68K se ejecutaba
con el intérprete C de Q68 (en PSP usaba un JIT de ensamblador MIPS,
`q68-jit-psp.S`, que en ARM no existe) repartido en 157 800 llamadas por
segundo. Resultado: el hilo principal se ahogaba — juego a ~0 fps, cargas
lentas y música arrastrada. (En v01.02 nada de esto corría: el dummy de
sonido entregaba ~85 muestras cada 55 líneas y el 68K era un stub.)

**Solución estructural: hilo de audio dedicado en otro núcleo.** La Vita
tiene 3 núcleos para aplicaciones y el emulador usaba uno. Ahora TODO el
audio (timers SCSP + 68K + mezcla + CDDA) corre en un hilo propio fijado
al tercer núcleo (`SCE_KERNEL_CPU_MASK_USER_2`), clockeado por el propio
puerto de audio (`sceAudioOutOutput` bloquea ~11.6 ms por chunk de 512
muestras — cadencia exacta de 44 100 Hz sin busy-wait):

- El hilo principal (SH2 + video) queda **tan libre como en v01.02**.
- La música suena **siempre a velocidad correcta**, aunque el video vaya
  por debajo de 60 fps: el 68K avanza en tiempo real en su núcleo
  (256 ciclos por muestra = 11.2896 MHz exactos, en rebanadas de 64
  muestras para buena granularidad de IRQ).
- Si una escena extrema retrasa la generación, el puerto mete un hueco de
  silencio y se recupera solo — nunca frena al juego.

**Sincronización entre hilos** (en `scsp.c`):

- `MINT` (interrupción SCSP→SCU): el hilo de audio no toca el SCU; deja
  `scsp_pending_mint` y el hilo principal la entrega en `ScspExec`.
- `M68KReset` desde SMPC (hilo principal): se difiere con un flag y lo
  aplica el hilo de audio antes de su siguiente paso.
- Realloc de búferes al cambiar PAL/NTSC: pausa el modo threaded y espera
  (spin acotado) a que el paso en curso termine.
- Escrituras SH2→SoundRAM concurrentes: `q68_touch_memory` es no-op sin
  JIT; los datos son arrays acotados — una carrera produce a lo sumo un
  click, jamás corrupción de memoria.

---

# Registro histórico v01.03 (arquitectura anterior)

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
