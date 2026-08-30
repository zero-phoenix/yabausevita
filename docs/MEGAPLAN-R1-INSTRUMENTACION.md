# Megaplan R1 — Instrumentación y automatización del ciclo

**Fecha:** 30-ago-2026 · **Repo activo:** `yabausevita-zp` (fork zero-phoenix)
**Estado:** aprobado para ejecución. Se ejecuta completo; cada fase termina con una
verificación explícita antes de pasar a la siguiente.

---

## Por qué este plan y no otro

La Ronda 0 midió: NiGHTS 17,1 FPS y el camino de render completo consume el
**1,27 %** del tiempo. El 98,7 % restante está en la emulación (SH-2, SCU,
SCSP/68K, CD) y ahí **no hay ni un contador**. La regla R8 de la bitácora fija
la ronda 1 como instrumentación, no optimización. Este plan ejecuta esa regla y,
de paso, elimina el único bloqueante de automatización que quedó en la Ronda 0:
el menú que exige pulsar Circle.

## Hallazgos previos que condicionan el plan

- **F-1** El `eboot.bin` instalado en Vita3K (`YABA00001`) es del **15-jul**,
  anterior al código actual. El código ya imprime `drawn/presented/dropped`
  (`vidgpu.c:253`) pero el binario instalado no. Desplegar la build nueva
  resuelve el hallazgo A9 sin tocar una línea de render.
- **F-2** `YabauseEmulate` (`yabause.c`) ya tiene `PROFILE_START/STOP` alrededor
  de cada subsistema — compilados a nada (`-DDONT_PROFILE`) y sobre `clock()`.
  La instrumentación reactiva ese esqueleto con `sceKernelGetProcessTimeWide()`
  (µs, ya usado en `main.c` y `vidgpu.c`).
- **F-3** En Vita, el SSH2 corre en **hilo propio** (`ssh2_thread_func`,
  paralelo real con el MSH2). Sus microsegundos se acumulan por separado y no
  son aditivos con MSH2: se reportan como solapamiento.
- **F-4** Con audio threaded (`ScspSetThreaded(1)`), el 68K/Q68 corre en el hilo
  de audio. El contador SCSP mide lo que ocurre en el hilo de emulación;
  el coste real del audio vive en su propio hilo y aquí no aparece. Sesgo
  declarado, no bug.
- **F-5** Los scripts de la sesión anterior (`vita3k_ctl.py`) murieron con su
  sesión. Lección: todo artefacto del ciclo vive en git, no en sesiones.

## Fases

### F1 — `autostart=1` en config.cfg (infraestructura del ciclo)

Cambio pequeño y aislado; no toca el lazo de emulación, no contamina A/B/C.

1. `vita_menu.h`: campo `int autostart;` en `VitaMenuConfig`.
2. `vita_menu.c` `load_config_file`: parsear `autostart=` (default 0).
3. `vita_menu.c` `save_config_file`: escribir la clave.
4. `vita_menu.c` `vita_menu_run`: tras cargar config y escanear BIOS, si
   `autostart && rom_path existe en disco` → fijar `rom_region` con
   `detect_rom_region`, emparejar BIOS por región si no hay uno explícito, y
   `return 0` **sin entrar al bucle del menú**. Si la ROM no existe, seguir al
   menú normal (fallback seguro).

**Verificación F1:** con `autostart=1` y rom válido, el log muestra
`Menu exited with result=0` inmediatamente tras `Entering menu`, sin espera de
pulsación. Con `autostart=0` el menú se comporta como siempre.

### F2 — Instrumentación de `YabauseEmulate` (la Ronda 1 propiamente dicha)

1. Nuevo par `src/vita/emuprof.c/.h`: 7 acumuladores (`msh2`, `ssh2`, `scu`,
   `scsp`, `vdp`, `cdb`, `smpc`) + API `EMUPROF_START/STOP/LOG/RESET` con macro
   de compilación para apagarlo entero.
2. `yabause.c`: sustituir los `PROFILE_START("X")` del lazo por los índices
   nuevos. El SSH2 se instrumenta dentro de `ssh2_thread_func` (su hilo).
3. `main.c`: al lado de `VIDGPUVdp2LogTiming()` del bloque de 5 s, llamar
   `EMUPROFLog()` + `EMUPROFReset()`. El log sale como:
   `EMU: msh2=..us ssh2=..us scu=..us scsp=..us vdp=..us cdb=..us smpc=..us`
   — totales de la ventana de 5 s, mismo contrato que GPU timing (R7).
4. Sesgo declarado: ~263 iteraciones/lazo × 7 pares de lecturas de timer por
   frame ≈ 3.700 lecturas/s a 20 FPS. En Vita real ~0,3-0,5 % de coste de
   medición; en Vita3K (que decide corrección, no rendimiento — R4) despreciable.

**Verificación F2:** el log imprime la línea `EMU:` cada 5 s; la suma
msh2+scu+scsp+vdp+cdb+smpc ≈ tiempo de pared no-render, y ssh2 se reporta
aparte por correr en paralelo.

### F3 — Compilar y desplegar

1. Commit (código + megaplan + bitácora parcial) en `main` de `yabausevita-zp`
   → push → CI (GitHub Actions, repo público, minutos ilimitados).
2. Descargar el artefacto `YabauseVita-vpk` con `gh`, extraer `eboot.bin` y
   reemplazar el de `ux0/app/YABA00001/` en el pref-path de Vita3K.
3. Verificar el despliegue por el formato del log: la primera línea
   `GPU: drawn=...` (formato nuevo) certifica binario nuevo.

**Verificación F3:** CI verde, VPK descargado, eboot reemplazado, formato de
log nuevo presente.

### F4 — Ojos y brazos: `tools/vita3k_ctl.py` en git

Recreación del controlador perdido, esta vez versionado en el repo:

1. Lanzar Vita3K **sin elevar** (vía `explorer.exe`) y con `Git\mingw64\bin`
   fuera del PATH del hijo (secuestro de DLL OpenSSL — A10).
2. Encontrar la ventana por título (`Vita3K`), traerla al frente
   (`AttachThreadInput` + `SetForegroundWindow`) y enviar scancodes
   (`SendInput`) — acotado a ventanas cuyo título empiece por `Vita3K`.
3. Editar `config.cfg` del pref-path (rom_path, autostart) por juego.
4. Vigilar `yabausevita_log.txt`, trocear por ventanas de 5 s y parsear
   `FPS:` / `GPU:` / `EMU:` a JSON estructurado.
5. Matar Vita3K al terminar cada corrida.

**Verificación F4:** una corrida end-to-end sin pulsación humana: launch →
autostart → 6+ ventanas de log → kill → JSON con métricas.

### F5 — Banco de pruebas (los tres juegos)

Por juego (Sonic R, Panzer Dragoon, NiGHTS), 2 corridas de ≥ 6 ventanas:

| Paso | Acción |
|---|---|
| 1 | `config.cfg`: rom_path del juego, `autostart=1`, cpu_mode=2, audio ON |
| 2 | Launch → 35 s de medición → kill |
| 3 | Parsear FPS mediana + GPU + EMU por ventana |
| 4 | Comprobar log sin `error`/`FATAL` |

Criterio de éxito: los tres juegos arrancan solos, producen ≥ 6 ventanas limpias
cada uno, y la línea `EMU:` identifica el subsistema dominante en los tres.
Ese número **define el blanco de la Ronda 2**.

Nota de homogeneidad: la línea base 17,1 FPS de NiGHTS se midió con el binario
del 15-jul. La build nueva (autostart + instrumentación) re-mide los tres juegos
y esa tabla pasa a ser la línea base oficial. La vieja queda como referencia.

### F6 — Limpieza de espacio (petición original del usuario)

- Ya movido por la sesión anterior: 31,81 GB en `D:\_DUPLICADOS` con
  inventario. No se borra nada sin orden expresa del usuario.
- Esta sesión: mover a `D:\_DUPLICADOS` los residuales seguros de CHD del
  pref-path (`_cache-corrupta`, `.bin` extraído de 524 MB junto al CHD que el
  emulador actual ya no usa — lee CHD directo). Antes de mover, verificar que
  nada los referencia.

### F7 — Cierre

1. Bitácora: entrada de Ronda 1 con las tablas, hallazgos nuevos (A12+) y
   reglas nuevas si emergen. Commit junto al código (como manda §6).
2. Suite MAGI (`pytest tests/ -q` en MAGI-System-IDE) en verde — el módulo
   `bitacora` ya vive ahí con sus tests.
3. Informe final: tabla comparativa, subsistema dominante, propuesta de blanco
   para Ronda 2.

## Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Push a `zero-phoenix/yabausevita` sin credencial | Probar push temprano (F3); si falla, misma build vía `davidchaveznge-wq` |
| CI tarda o falla | El workflow ya existía y compiló el 15-jul; si falla, el log de errores sube como artefacto |
| El autostart rompe el arranque normal | Fallback: sin `autostart=1` el flujo es byte a byte el de antes; key nueva ignorada por configs viejas |
| Sobrecoste de medición en Vita real | Macro de compilación para apagarlo; sesgo estimado < 0,5 % y declarado |
| Carrera MSH2/SSH2 en los acumuladores | Slots distintos por hilo; SSH2 acumula solo dentro de su hilo mientras el principal espera su semáforo |
