# CHD: lectura directa de sectores (v01.01)

## Qué estaba mal antes

El flujo anterior, al seleccionar un `.chd` en el menú, hacía esto en el
hilo principal, sin dibujar ni un frame:

1. Descomprimía **el disco completo** (400-700 MB) a `juego.bin` en `ux0:`.
2. Releía ese archivo entero y lo reescribía sector a sector (2448→2352)
   a un `.tmp`, borraba el original y lo renombraba.

En una Vita eso son **varios minutos con la pantalla congelada** y sin
respuesta a botones — exactamente el síntoma reportado. Además:

- El codec **cdfl (FLAC)** era un stub: llenaba de ceros los hunks de
  audio (y chdman lo usa por defecto). Silencio o datos corruptos.
- Los hunks **SELF** (duplicados) se leían desde un descriptor abierto
  solo-escritura → basura en el `.bin` resultante.
- El `.bin` aplanado perdía el **TOC**: sin pistas de audio, el CD block
  del Saturn recibía un disco de una sola pista.
- Si el usuario cerraba la app a mitad de extracción, el `.bin` parcial
  quedaba en disco y se **reutilizaba como caché** en el siguiente
  intento: cuelgue instantáneo.

## Qué hace ahora

`src/vita/cd_chd.c` implementa un `CDInterface` (`CDCORE_CHD`) que lee
sectores **directamente del CHD** usando **libchdr** (la misma librería
que usan los cores de RetroArch en PS Vita):

- `chd_open()` + metadatos `CHT2`/`CHTR` → tabla de pistas + TOC Saturn
  real (datos 0x41, audio 0x01, leadout correcto).
- `ReadSectorFAD(fad)` → localiza pista → frame físico CHD (con el
  relleno de 4 frames entre pistas y la semántica de pregap `V`/virtual
  de MAME) → decodifica ese hunk con caché LRU de 8 hunks (~157 KB RAM).
- Codecs soportados: cdlz (LZMA), cdzl (deflate), cdfl (FLAC via
  dr_flac), cdzs (zstd), huffman, none, SELF y detección de parents.
- ECC/sync regenerados por libchdr (`WANT_RAW_DATA_SECTOR`), audio con
  el byte-swap correcto (CHD guarda big-endian, Saturn espera little).

Resultado: **carga instantánea**, cero espacio extra en `ux0:`, y datos
byte-exactos.

## Verificación

`tools/chd_verify.c` (se compila en PC, no en la Vita):

```sh
gcc -O2 -DCHD_HOST_TEST \
  -Isrc/vita/libchdr/include \
  -Isrc/vita/libchdr/deps/lzma-25.01/include \
  -Isrc/vita/libchdr/deps/miniz-3.1.1 \
  -Isrc/vita/libchdr/deps/zstd-1.5.7 \
  src/vita/libchdr/unity.c src/vita/cd_chd.c tools/chd_verify.c \
  -o chd_verify -lm

./chd_verify gen /tmp/t                # genera test.bin + test.cue
chdman createcd -i /tmp/t/test.cue -o /tmp/t/test.chd
./chd_verify check /tmp/t/test.chd /tmp/t/test.bin
```

El disco sintético tiene una pista de datos MODE1 (con ECC válido, que
chdman "strippea" y el lector regenera), una pista de audio senoidal
(codec FLAC), una de ruido (LZMA/deflate), un pregap virtual
(`PREGAP 00:02:00`) y un pregap en archivo (`INDEX 00`). Resultado con
chdman 0.264:

```
Sectores verificados: 5000 (de ellos 150 de silencio)
Diferencias: 0
RESULTADO: OK — lectura CHD byte-exacta en todo el disco
```

## Archivos eliminados

`chd_read.c/h`, `lzma_dec.c/h`, `huffman.c/h`, `bitstream.c/h`,
`7zTypes.h`, `Compiler.h`, `Precomp.h` — reemplazados por
`src/vita/libchdr/` (vendorizado, un solo translation unit `unity.c`).
