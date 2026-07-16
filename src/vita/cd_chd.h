/*
 * cd_chd.h — YabauseVita: lector directo de CHD (v1-v5) via libchdr
 *
 * Reemplaza la extracción completa a .bin temporal (que congelaba la
 * consola varios minutos y corrompía los datos) por lectura de
 * sectores bajo demanda con caché de hunks.
 */
#ifndef CD_CHD_H
#define CD_CHD_H

#ifdef CHD_HOST_TEST
/* Compilación en PC para el harness de verificación */
#include <stdint.h>
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  s32;
typedef struct
{
        int id;
        const char *Name;
        int (*Init)(const char *);
        int (*DeInit)();
        int (*GetStatus)();
        s32 (*ReadTOC)(u32 *TOC);
        int (*ReadSectorFAD)(u32 FAD, void *buffer);
} CDInterface;
#else
#include "../cdbase.h"
#endif

#define CDCORE_CHD 3

extern CDInterface CHDCD;

#endif /* CD_CHD_H */
