# Bitácora de optimización de YabauseVita

Documento acumulativo. Cada ronda de mejora **añade**, nunca reemplaza. Su razón de
existir es que la ronda N+1 empiece sabiendo todo lo que aprendieron las rondas
anteriores — incluido lo que aprendieron las propuestas que perdieron.

> **Regla que sostiene todo esto:** una propuesta que pierde deja conocimiento igual
> que una que gana. Perder por «no mejoró» y perder por «rompió el render» son cosas
> distintas, y la segunda enseña más. Si solo se guarda la ganadora, la ronda
> siguiente vuelve a pagar por descubrir lo mismo.

---

## 1. El instrumento ya existe

No hay que construir telemetría: `src/vita/main.c` y `src/vita/vidgpu.c` ya la traen.
Cada 5 segundos el lazo de emulación escribe al log:

```
FPS: %.1f
GPU: drawn=%d presented=%d dropped=%d composite=%lldus upload=%lldus display=%lldus
```

Y eso descompone el coste del fotograma en tres etapas medidas por separado:

| Campo | Qué mide | Qué significa si domina |
|---|---|---|
| `composite` | Rasterizado por software de VDP1+VDP2 en CPU | El cuello está en **hacer** los píxeles |
| `upload` | Transferencia del búfer de CPU a textura GPU | El cuello está en **mover** los píxeles |
| `display` | Presentación / swap | El cuello está en la **sincronización** con el vsync |
| `dropped` | Fotogramas descartados porque el hilo de render seguía ocupado | El cuello está en el **reparto entre núcleos** |
| `drawn` vs `presented` | Cuántos se dibujaron vs. cuántos llegaron a pantalla | Mide el desperdicio real |

**Consecuencia de método:** ninguna ronda propone nada antes de leer estos números.
Optimizar antes de medir es adivinar, y con tres etapas separadas no hace falta.

### Configuración de la medición

- Juegos del banco: `Sonic R (Europe)`, `Panzer Dragoon (Europe)`, `NiGHTS into Dreams`
- Los tres en CHD, en `ux0:data/yabause/roms/`
- CPU: `VMENU_CPU_DYNARM` (dynarec ARM) salvo que la ronda diga otra cosa
- Audio: **encendido**, porque apagarlo cambia el reparto de núcleos y falsea la medida
- Auto-frameskip: encendido (`yinit.frameskip = 1`), como en uso real

---

## 2. Las tres filosofías

Las tres propuestas de cada ronda no son tres ideas sueltas: son **tres formas
distintas de atacar el mismo cuello**, y son mutuamente excluyentes por diseño. Si
las tres apuntaran al mismo mecanismo, la comparación no distinguiría nada.

### A — Hacer menos trabajo
> *El píxel más rápido es el que no se dibuja.*

Ataca `composite`. Elimina cómputo redundante sin cambiar el resultado visible:
planos que no cambiaron entre fotogramas, scanlines idénticas, capas ocultas bajo
otras, regiones fuera del área visible.

**Riesgo característico:** invalidación incompleta. El fallo típico no es un crash,
es un fantasma que aparece tres minutos después.

### B — Mover menos datos
> *El bus es un recurso, igual que el reloj.*

Ataca `upload`. Un fotograma de 704×512 a 16 bits son ~700 KB por el bus, 60 veces
por segundo. Subir solo el rectángulo sucio, o mapear la memoria del rasterizador
directamente como textura (`sceGxmMapMemory`) y eliminar la copia entera.

**Riesgo característico:** carreras entre el rasterizador y la GPU al compartir
memoria; tearing si desaparece la doble memoria intermedia.

### C — Repartir mejor entre núcleos
> *La Vita tiene tres núcleos utilizables y el juego usa uno y medio.*

Ataca `dropped`. Hoy hay un hilo de render en el núcleo 1 y el audio en otro. Si
`dropped` es alto, el render no da abasto: mover el composite al hilo de render, o
partirlo en bandas horizontales entre dos núcleos.

**Riesgo característico:** el reparto que va bien en un juego va mal en otro. Es la
categoría donde más importa medir con los tres juegos y no con uno.

---

## 3. Criterio de decisión

Fijado **antes** de ver los números. Tres condiciones en orden; la primera que falla
descarta la propuesta.

1. **Compila.** Sin `.vpk` no hay candidata; el error de compilación es el informe.
2. **No retrocede.** Sin excepciones nuevas en el log, y el framebuffer de referencia
   idéntico en los tres puntos de captura del guion.
3. **Mejora sostenida.** Mediana de tres corridas, ≥ 5 % en la métrica declarada, y
   que la peor corrida de la candidata siga siendo mejor que la mejor de la línea
   base. Una mejora que solo aparece en la corrida afortunada no es una mejora.

**Empate:** gana la de menor riesgo declarado, no la de mejor número.
**Nadie pasa:** no se publica nada y la ronda se cierra con su informe. Un ciclo que
siempre produce un ganador es un ciclo que aprendió a mentir.

---

## 4. Rondas

### Ronda 0 — Línea base · **EJECUTADA 30-ago-2026**

Medido en Vita3K build 4058, `cpu_mode=2` (dynarec ARM), audio ON, BIOS real,
21 ventanas de 5 s, sin errores en el log.

| Juego | FPS mediana | composite | upload | display | frames/ventana |
|---|---|---|---|---|---|
| NiGHTS into Dreams | **17,1** | 41.400 µs | 15.385 µs | 5.924 µs | 86 |
| Sonic R | pendiente | — | — | — | — |
| Panzer Dragoon | pendiente | — | — | — | — |

#### El resultado que cambia el plan

Los tres números son **totales de la ventana de 5 s**, no medias por fotograma.
Sumados: 62.709 µs sobre 5.000.000 µs de pared.

> **El camino de render consume el 1,27 % del tiempo.**

Dentro de ese 1,27 %, el reparto es composite 66 %, upload 24,5 %, display 9,4 %.
Pero el reparto interno es irrelevante frente a la magnitud: **eliminar el render
entero subiría de 17,1 a ~17,3 FPS.**

El 98,7 % restante está en emulación — SH-2, SCU, SCSP/68K o la capa de CD — y ahí
no hay instrumentación todavía.

#### Consecuencia directa

**Las tres filosofías de la §2 quedan en suspenso.** Las tres atacan el camino de
render, que es donde no está el problema. La ronda 1 no puede proponer nada hasta
que exista telemetría del lado de la emulación: contadores por subsistema en
`YabauseExec`, o como mínimo separar tiempo de CPU SH-2 del resto del fotograma.

Esto es exactamente para lo que existe la ronda 0. Sin ella se habrían gastado tres
ciclos de compilación optimizando el 1,27 %.

### Ronda 1 — Instrumentación y los cinco bloqueos · **EJECUTADA 30-ago-2026 (tarde)**

**Fecha:** 30-ago-2026 · **Línea base:** commit `575b990` · **Métrica objetivo:** desglose EMU por subsistema + imagen verificada

#### Qué se hizo (era ronda de instrumentación, R8; se convirtió en ronda de rescate)

1. `autostart=1` en config.cfg: el menú se salta si rom_path existe y la BIOS
   valida (mismo flujo que la selección manual). Las rondas ya no exigen
   pulsar Circle. Sin `autostart`, byte a byte el comportamiento de antes.
2. `emuprof` (`src/vita/emuprof.c/.h`): 10 acumuladores en µs por ventana de
   5 s volcados como `EMU: msh2= ssh2= scu= scsp= scsp_th= m68k= hblank=
   vdp= cdb= smpc=` junto al FPS. Reactiva el esqueleto PROFILE_* que
   DONT_PROFILE apagaba, con `sceKernelGetProcessTimeWide()` en vez de `clock()`.
   `scsp_th` acumula desde el hilo de audio (µs paralelos, no aditivos).
3. **FPS del ROM en pantalla** (`show_fps`): contador del FPS emulado dibujado
   sobre el juego (`vidgpu.c`, tira de glifos 8×8). Antes `g_show_fps` era
   código muerto. Ahora se distinguen a simple vista los FPS de la app Vita
   (los del título de Vita3K) de los del juego Saturn.
4. `tools/vita3k_ctl.py` versionado en git: lanza Vita3K sin elevar y sin el
   OpenSSL de Git en el PATH, arranca YABA00001 con `-r`, edita config.cfg
   **sin BOM** (con BOM el emulador ignora rom_path en silencio), pulsa
   teclas, y hace **capturas continuas de la ventana del juego con veredicto
   de imagen (negro %) y movimiento (diff %)**.

#### Los cinco bloqueos encontrados y resueltos (cada uno con evidencia)

| # | Bloqueo | Evidencia | Fix |
|---|---|---|---|
| B1 | CI roto desde el 23-ago: `./vdpm` del checkout espera pacman en `bin/`, el bootstrap nuevo lo instala en `libexec/vdpm/` | log del run 33330242639 | usar el `vdpm` empaquetado del PATH; validado en Docker antes de pushear |
| B2 | Flags `-Ofast -flto -ffast-math -mfpu=neon` (commit a674198) | el dynarec colgaba; hipótesis razonable, falsada: seguía colgado con `-O3` | revertido a `-O3` plano (lo único validado en runtime) |
| B3 | **`sh2fast.c`/`sh2lru.c` del zp ejecutaban ~2 % del trabajo real** | con ellos: msh2=0,1 s/ventana y «60 FPS»; con los de david: msh2≈2 s/ventana y 40-46 FPS | restaurados los de `yabausevita` (davidchaveznge-wq) |
| B4 | Núcleos VDP de Kronos (vdp1/vdp2/vidshared) incompatibles con vidgpu | pantalla negra con la cadena Kronos; los commits de «revert headers» ya avisaban | restaurada la cadena VDP completa del árbol david |
| B5 | **Región de BIOS**: saturn_bios.bin no-US con disco US → TVMD.DISP nunca pasa a 1 → negro absoluto | sonda: `tvmd=0001 nonzero=0` vs `tvmd=8101 nonzero=647` con BIOS USA | emparejar BIOS por región (`auto_bios=1` lo hace; el autostart lo replica) |

Nota sobre B2: perdedor útil — los flags agresivos no causaban el hang del
dynarec, pero `-ffast-math` sobre un emulador que compara floats bit a bit
seguía siendo un riesgo sin contrapartida medida. Fuera.

#### El hallazgo que invalidó media ronda: el FPS mentía

Con B3 sin arreglar, el emulador reportaba 59,9-60,0 FPS estables, drawn=296,
presented=296 — y la pantalla era negra y la BIOS no progresaba. El contador
de FPS cuenta llamadas a `YabauseExec`, no trabajo emulado: un intérprete
roto que no ejecuta instrucciones también «hace 60 FPS». De ahí la regla R9.

#### Medición final (build: david-SH2 + david-VDP + CHD david + BIOS por región, cpu_mode=0, audio ON, 11 ventanas/juego)

| Juego | FPS med. | Imagen (negro %) | Movimiento (diff %) | drawn/5 s | msh2+ssh2 s/5 s |
|---|---|---|---|---|---|
| Sonic R (EU) | 46,3 | ✅ 18,4 % | ✅ 5,0 | 59 | 1,36+1,95 |
| Panzer Dragoon (EU) | **59,8** | ✅ 42,2 % | ✅ **45,9** | 248 | 2,48+0,36 |
| NiGHTS (USA) | 40,0 | parcial 86,8 % | arranque lento | 21 | 1,89+2,41 |

- Panzer Dragoon corre a **velocidad completa** (59,8 de 60) con imagen viva
  (45,9 % de píxeles cambiando entre capturas) y apenas usa el SH2 esclavo.
- Sonic R: imagen confirmada visualmente (pista, cielo, personaje), SH2-bound.
- NiGHTS: juego dual-CPU (usa los dos SH2 a tope), arranca más despacio;
  la pantalla de licencia aparece (`nonzero=647`) pero a 40 FPS su boot es
  largo. Queda pendiente verlo llegar al título con una espera mayor.
- El audio threaded cuesta 1,1-1,4 s/5 s de un segundo núcleo (scsp_th).

#### Qué queda sin comprobar

- El dynarec (`SH2DynARM`, cpu_mode=2) **sigue colgado al primer frame** con
  `-O3`: cache JIT OK (8 MB RWX), el hang está en la ejecución del código
  generado. No se ha probado en hardware real — Vita3K decide corrección,
  no rendimiento (R4); quizá en Vita real funcione.
- NiGHTS no llegó al título en 75 s. Repetir con espera ≥ 3 min.
- Controles: no se verificó que el input cruce al juego (la prueba con ENTER
  se hizo sobre una corrida que había caído al menú — no cuenta).
- La medición es Vita3K sobre x86 (R4): el reparto de subsistemas orienta,
  la cifra absoluta no es la de la Vita.

#### Hallazgos reutilizables (nuevos)

- A12 · El FPS del log cuenta `YabauseExec` por segundo, no trabajo emulado.
  Un core roto «hace 60 FPS». Solo vale junto a verificación de imagen.
- A13 · Los núcleos SH2 del zp (sh2fast/sh2lru) estaban rotos: ejecutaban
  ~2 % del trabajo. Los de david son la referencia validada.
- A14 · La cadena VDP de Kronos no es intercambiable con vidgpu: mismo
  «funciona» aparente en log, pantalla negra en realidad.
- A15 · La BIOS de Saturn no enciende TVMD.DISP con un disco de otra región:
  el síntoma es negro absoluto con emulación «corriendo». Emparejar región.
- A16 · config.cfg con BOM: `sscanf("%63[^=]")` lee `\ufeffrom_path` y
  rom_path se ignora en silencio. Escribir siempre UTF-8 sin BOM.
- A17 · Vita3K abre DOS ventanas (GUI + juego 960×544). Capturar/pulsar
  teclas por PID del proceso y tamaño de cliente, no por título.
- A18 · Con BIOS correcta, Panzer Dragoon ya va a velocidad completa en
  Vita3K. El techo real de mejora está en Sonic R y NiGHTS (SH2-bound).

#### Reglas nuevas

- R9 · **Ninguna corrida se acepta sin verificación de imagen y movimiento**
  (capturas continuas + diff %). El FPS por sí solo no es evidencia (A12).
- R10 · Todo cambio de core (SH2, VDP) se valida con corrida + capturas
  antes de llamarlo «funciona». El log no puede ver lo que ve el jugador.
- R11 · El `show_fps` en pantalla es del ROM; el del título de Vita3K es de
  la app. No citarlos indistintamente.

### Ronda 2 — NiGHTS, input, dynarec y el perfil SH2 · **EJECUTADA 30-ago-2026 (noche)**

**Fecha:** 30-ago-2026 · **Línea base:** release `v0.9.11-r1` (commit `3d499fe`) · **Métrica:** veredictos R9 de los cuatro experimentos pendientes

Ejecutada con `scripts/ronda_emulador.py` de MAGI — la metodología de la
ronda 1 convertida en procedimiento, supervisada con doble verificación.

#### 1. NiGHTS con espera larga (185 s) — NO LLEGA AL TÍTULO

Imagen estable (86,8 % negro), movimiento 0,16 %, FPS 40,6, drawn=23/ventana,
sin errores. Análisis de píxeles de la captura: el contenido no negro es
texto blanco (353 muestras cuantizadas) + rojo (221) en dos bandas — la
**pantalla de licencia SEGA** (texto blanco, logo rojo). La BIOS enciende el
vídeo (tvmd=8101, ronda 1) y queda esperando al disco: cdb=49-60 ms/ventana
de actividad continua. Sonic R y Panzer (ambos EU) arrancan con el mismo
mecanismo; NiGHTS (USA, edición «(RE)», 21 pistas, leadout 233783) no.
**Sospechoso: el camino de lectura de ESTE disco** — pista 1 MODE1_RAW con
start_fad=150 — no la región (la BIOS ya es USA). Es el blanco de la ronda 3.

#### 2. Input — NO CRUZA (con matiz de diseño)

El experimento v5.11.0 midió diff antes/después en el attract de Sonic R,
que YA se mueve: delta −0,27 % y un veredicto falso de «no cruza» sin
aislar la pulsación. Se rediseñó (v5.12.0): capturas a 1 s, PICO de
transición, y CONTROL de 6 s sin pulsar. Resultado correcto: pico control
5,16 %, pico tras ENTER 5,31 % — la pulsación no produce transición
visible. **Matiz:** el attract puede ignorar START; queda probar en una
pantalla que responda (título con PRESS START). No se declara «el teclado
no llega al pad» — se declara lo medido.

#### 3. Dynarec — hang confirmado por tercera build

cpu_mode=2, 90 s: cero ventanas de log. Tercera build independiente que
reproduce el hang al primer frame (con `-Ofast`, con `-O3`, y con el VPK
oficial del CI). Cache JIT OK (8 MB RWX). La ejecución del código generado
nunca completa un frame. Pendiente hardware real (R4).

#### 4. Perfil SH2 — SH2-BOUND confirmado

| Juego | FPS | % del hilo principal en msh2+ssh2 | vision_ok |
|---|---|---|---|
| Sonic R | 44,5 | **69,9 %** | ✅ |
| Panzer Dragoon | 59,8 | **64,7 %** | ✅ |
| NiGHTS | 41,4 | **90,7 %** | (licencia) |

El hilo de audio (scsp_th) consume 1,16-1,67 s por ventana de 5 s en
paralelo. **El blanco de la ronda 3 es el par SH2** — y Panzer demuestra
que 64,7 % SH2 aún da para 59,8 FPS: hay margen de ejecución, el problema
es cuánto trabajo generan los intérpretes.

#### Qué queda sin comprobar

- Por qué la BIOS no valida el disco de NiGHTS (¿FAD/track de la pista 1?).
- Input en pantalla que responda a START.
- Dynarec en Vita real.
- Los µs de SH2 no son aditivos con scsp_th (hilos paralelos) — el perfil
  del hilo principal no cubre el segundo núcleo.

#### Hallazgos reutilizables

- A19 · NiGHTS se queda en la pantalla de licencia (texto blanco + logo
  rojo sobre negro) con cdb activo: la BIOS espera al disco indefinidamente.
  No es región (BIOS ya correcta) — es el disco o su lectura.
- A20 · Medir input con diff antes/después en una escena que ya se mueve no
  aisla la pulsación. Protocolo correcto: pico de transición + control sin
  pulsar.
- A21 · El attract puede ignorar START: un «no cruza» en attract no prueba
  que el teclado no llegue al pad. Declarar el estado de la pantalla en el
  veredicto.
- A22 · Panzer a 59,8 FPS con 64,7 % SH2: el cuello no es solo el
  porcentaje SH2, es el coste por instrucción de los intérpretes.

#### Reglas nuevas

- R12 · Un experimento de input se diseña con control sin pulsar y pico de
  transición, nunca con delta de medianas en escena en movimiento (A20).
- R13 · Todo veredicto declara en qué pantalla se midió; «no cruza» en
  attract es «no respondió ESTA pantalla» (A21).

<!-- Plantilla para cada ronda siguiente. Copiar y rellenar.

### Ronda N — <título corto de lo que se atacó>

**Fecha:** · **Línea base:** commit `xxxxxxx` · **Métrica objetivo:** `<campo>`

#### Hipótesis
| | Filosofía | Cambio propuesto | Predicción falsable |
|---|---|---|---|
| A | Hacer menos | | `composite` baja >= X % |
| B | Mover menos | | `upload` baja >= X % |
| C | Repartir mejor | | `dropped` baja >= X % |

#### Medición
| | Compila | Regresión | FPS | Métrica | Veredicto |
|---|---|---|---|---|---|
| A | | | | | |
| B | | | | | |
| C | | | | | |

#### Ganadora y por qué
<qué ganó, por cuánto, y por qué perdió cada una de las otras dos en términos de las
tres condiciones>

#### Qué queda sin comprobar
<lo que no se midió. «No lo miré» no es «está bien».>

#### Hallazgos reutilizables
<de las TRES propuestas, incluidas las perdedoras>

-->


---

## 5. Conocimiento acumulado

Esta sección es la que hace que la bitácora valga más que la suma de sus rondas.
Cada entrada nueva se añade abajo con su ronda de origen; **nada se borra**, solo se
marca como superado si una medición posterior lo contradice.

### 5.1 Sobre la arquitectura (lo que ya sabemos del código)

| # | Hallazgo | Origen |
|---|---|---|
| A1 | `VIDCORE_GPU` no es un rasterizador GPU: rasteriza por software (`VIDSoftVdp1/Vdp2`) y usa la GPU solo para subir y presentar. Cualquier propuesta que asuma rasterizado en GPU parte de una premisa falsa. | Lectura de `vidgpu.c`, 30-ago-2026 |
| A2 | Ya hay hilo de render en un núcleo aparte con doble búfer, y descarta fotogramas cuando se satura (`dropped_presents`). El paralelismo básico ya está hecho; la mejora está en el reparto, no en «añadir hilos». | `vidgpu.c:VIDGPUVdp2DrawEnd` |
| A3 | El audio ya corre en hilo dedicado (`ScspSetThreaded(1)`) con Q68 ejecutando el driver de sonido del juego. Apagar el audio libera núcleo y falsea cualquier medida comparativa. | `main.c` |
| A4 | El auto-frameskip de `vdp2.c` está activo y depende de `HAVE_GETTIMEOFDAY`. Sin esa macro, `YabauseGetTicks` devuelve basura en Vita — un fallo ya pagado una vez. | `main.c`, comentario en el código |
| A5 | Hay tres modos de CPU seleccionables (`DYNARM`, `SH2LRU`, `SH2Fast`). Comparar propuestas con modos distintos invalida la comparación. | `main.c:cfg.cpu_mode` |
| A7 | **El camino de render es el 1,27 % del tiempo** (62,7 ms de cada 5 s). Optimizar composite, upload o display no puede dar más de ~0,2 FPS sobre 17,1. El cuello está en emulación. | Ronda 0, NiGHTS, 30-ago-2026 |
| A8 | Los tres números de `GPU timing` son **totales de la ventana de 5 s**, no medias por fotograma. Leerlos como µs/frame invierte la conclusión por completo. | Ronda 0 |
| A9 | Esta build imprime `GPU timing: composite/upload/display/frames`. **No** imprime `drawn`, `presented` ni `dropped`, aunque `vidgpu.c` los lleva. La filosofía C no tiene métrica hasta que se expongan. | Ronda 0 |
| A10 | Arrancar el emulador exige cinco cosas que no son obvias: PATH sin `Git\mingw64\bin` (o Vita3K coge su OpenSSL y muere con `EVP_MD_CTX_get_size_ex`), lanzar sin elevar, caché `.bin` válida, `auto_bios=0` con `bios_path` explícito, y `cpu_mode=2`. Con `auto_bios=1` cae a HLE y `YabauseInit` devuelve −1. | Puesta en marcha, 30-ago-2026 |
| A11 | La extracción CHD→`.bin` cuesta ~524 MB en C: por juego y se corrompe si el disco se queda sin espacio. Una caché truncada da `Unsupported CD image` (−2), no un error de disco. | Ronda 0 |
| A12 | El FPS del log cuenta llamadas a `YabauseExec`, no trabajo emulado: un intérprete roto «hace 60 FPS». Solo vale junto a verificación de imagen. | Ronda 1 |
| A13 | Los núcleos SH2 del zp (sh2fast/sh2lru) estaban rotos (~2 % del trabajo real). Los del árbol davidchaveznge-wq son la referencia validada. | Ronda 1 |
| A14 | La cadena VDP de Kronos no es intercambiable con vidgpu: en log «funciona», en pantalla negro. | Ronda 1 |
| A15 | La BIOS de Saturn no enciende TVMD.DISP con disco de otra región: negro absoluto con la emulación «corriendo». Emparejar región siempre. | Ronda 1 |
| A16 | config.cfg con BOM: el sscanf lee `\ufeffrom_path` y rom_path se ignora en silencio. Escribir UTF-8 sin BOM. | Ronda 1 |
| A17 | Vita3K abre dos ventanas (GUI y juego 960×544). Capturar y pulsar teclas por PID y tamaño de cliente, no por título. | Ronda 1 |
| A18 | Con BIOS correcta, Panzer Dragoon ya corre a velocidad completa (59,8 FPS) en Vita3K. El margen real está en Sonic R y NiGHTS (SH2-bound). | Ronda 1 |
| A19 | NiGHTS se queda en la pantalla de licencia (texto blanco + logo rojo) con cdb activo: la BIOS espera al disco indefinidamente. No es región — es el disco o su lectura. | Ronda 2 |
| A20 | Medir input con diff antes/después en escena en movimiento no aísla la pulsación. Protocolo correcto: pico de transición + control sin pulsar. | Ronda 2 |
| A21 | El attract puede ignorar START: «no cruza» en attract es «esta pantalla no respondió», no «el teclado no llega». | Ronda 2 |
| A22 | Panzer a 59,8 FPS con 64,7 % SH2: el cuello no es solo el porcentaje, es el coste por instrucción de los intérpretes. | Ronda 2 |
| A6 | `PORTING_NOTES.md` describe el hito 0 y **está desactualizado**: dice que SH-2 va en intérprete puro y que vídeo, sonido, mando y CD están en DUMMY. El código real tiene dynarec ARM, CHD, audio Vita y renderizador GPU. No usarlo como fuente. | Contraste con `src/vita/main.c`, 30-ago-2026 |

### 5.2 Reglas derivadas (lo que no hay que volver a intentar)

| # | Regla | Por qué |
|---|---|---|
| R1 | No proponer un JIT nuevo de SH-2. Ya existe `SH2DynARM`. | A5 |
| R2 | No medir con el audio apagado. | A3 |
| R3 | No comparar dos propuestas con distinto `cpu_mode`. | A5 |
| R4 | No citar los FPS de Vita3K como prueba de rendimiento — es HLE sobre x86, sus tiempos no son los del Cortex-A9. Vita3K decide **corrección**; las métricas internas deciden **rendimiento**. | Diseño del ciclo |
| R5 | No leer `PORTING_NOTES.md` para saber el estado del emulador. Leer `src/vita/main.c`. | A6 |
| R6 | **No proponer optimizaciones del camino de render** (composite, upload, display) hasta que exista telemetría de emulación que demuestre que ahí queda algo. Es el 1,27 % del tiempo: el techo de la mejora es ~0,2 FPS. | A7 |
| R7 | No interpretar `GPU timing` como µs por fotograma. Son totales de 5 s. | A8 |
| R8 | La ronda 1 es de **instrumentación**, no de optimización: contadores por subsistema en `YabauseExec` (SH-2, SCU, SCSP/68K, CD). Sin eso, cualquier propuesta apunta al 98,7 % a ciegas. | A7 |
| R9 | Ninguna corrida se acepta sin **verificación de imagen y movimiento** (capturas continuas + diff %). El FPS por sí solo no es evidencia. | A12, Ronda 1 |
| R10 | Todo cambio de core (SH2, VDP) se valida con corrida + capturas antes de llamarlo «funciona». El log no puede ver lo que ve el jugador. | A13, A14 |
| R11 | El `show_fps` en pantalla mide el ROM; el del título de Vita3K mide la app. No citarlos indistintamente. | Ronda 1 |
| R12 | Un experimento de input se diseña con control sin pulsar y pico de transición, nunca con delta de medianas en escena en movimiento. | A20 |
| R13 | Todo veredicto declara en qué pantalla se midió; «no cruza» en attract es «no respondió ESTA pantalla». | A21 |

---

## 6. Cómo lo usa MAGI

El enjambre lee este documento **entero** al empezar cada ronda, antes de proponer.

- **Melchior** redacta las tres propuestas. Debe declarar a qué filosofía pertenece
  cada una y su predicción falsable. Si alguna choca con una regla de §5.2, se
  rechaza sin llegar a compilar.
- **Balthasar** ejecuta y refuta. Su trabajo no es opinar sobre las propuestas sino
  compilarlas, correrlas y traer números.
- **Casper** aplica el criterio de §3 y redacta la entrada de la ronda, incluidos los
  hallazgos reutilizables de las tres.
- Al cerrar, la ronda **se escribe en este archivo** y se sube con el mismo commit
  que el cambio ganador. La medición viaja pegada al código que la forzó.
