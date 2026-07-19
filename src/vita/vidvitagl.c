#include <string.h>
#include <stdint.h>
// #include <vitaGL.h> // Se incluirá más adelante cuando la libería esté instalada
#include "yabause.h"
#include "vdp1.h"

#define VIDCORE_VITAGL_ID 5

extern int vita_log(const char *fmt, ...);

/* --- Reused stubs from VIDSoft (until hardware rendering is fully implemented) --- */
extern int VIDSoftInit(void);
extern void VIDSoftDeInit(void);
extern void VIDSoftResize(unsigned int, unsigned int, int);
extern int VIDSoftIsFullscreen(void);
extern int VIDSoftVdp1Reset(void);
extern void VIDSoftVdp1DrawStart(void);
extern void VIDSoftVdp1DrawEnd(void);
extern void VIDSoftVdp1NormalSpriteDraw(void);
extern void VIDSoftVdp1ScaledSpriteDraw(void);
extern void VIDSoftVdp1DistortedSpriteDraw(void);
extern void VIDSoftVdp1PolygonDraw(void);
extern void VIDSoftVdp1PolylineDraw(void);
extern void VIDSoftVdp1LineDraw(void);
extern void VIDSoftVdp1UserClipping(void);
extern void VIDSoftVdp1SystemClipping(void);
extern void VIDSoftVdp1LocalCoordinate(void);
extern int VIDSoftVdp2Reset(void);
extern void VIDSoftVdp2DrawStart(void);
extern void VIDSoftVdp2DrawEnd(void);
extern void VIDSoftVdp2DrawScreens(void);
extern void VIDSoftVdp2SetResolution(u16);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG0(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG1(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG2(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG3(int);
extern void FASTCALL VIDSoftVdp2SetPriorityRBG0(int);
extern void VIDSoftOnScreenDebugMessage(char *string, ...);
extern void VIDSoftGetGlSize(int *width, int *height);


/* --- New vitaGL implementations --- */

static int VIDVitaGLInit(void)
{
    vita_log("[VIDVitaGL] Initializing vitaGL core...\n");
    
    // Aquí inicializaremos vitaGL: vglInit(0);
    // Configuración de texturas, shaders y búferes VBO para los quads.
    
    return VIDSoftInit(); // Fallback temporal a la inicialización soft
}

static void VIDVitaGLDeInit(void)
{
    vita_log("[VIDVitaGL] De-initializing vitaGL core...\n");
    
    // vglEnd();
    VIDSoftDeInit();
}

static void VIDVitaGLVdp1DrawEnd(void)
{
    // Aquí es donde empujaremos los vértices recolectados a la GPU
    // vglDrawArrays(GL_QUADS, 0, vertex_count);
    // vglSwapBuffers(GL_FALSE);
    
    VIDSoftVdp1DrawEnd(); // Fallback temporal
}

VideoInterface_struct VIDVitaGL = {
    VIDCORE_VITAGL_ID,
    "vitaGL Hardware Video Interface",
    VIDVitaGLInit,
    VIDVitaGLDeInit,
    VIDSoftResize,
    VIDSoftIsFullscreen,
    VIDSoftVdp1Reset,
    VIDSoftVdp1DrawStart,
    VIDVitaGLVdp1DrawEnd,
    VIDSoftVdp1NormalSpriteDraw,     // Próximamente: interceptar estos comandos
    VIDSoftVdp1ScaledSpriteDraw,     // para generar arrays de vértices OpenGL
    VIDSoftVdp1DistortedSpriteDraw,
    VIDSoftVdp1PolygonDraw,
    VIDSoftVdp1PolylineDraw,
    VIDSoftVdp1LineDraw,
    VIDSoftVdp1UserClipping,
    VIDSoftVdp1SystemClipping,
    VIDSoftVdp1LocalCoordinate,
    VIDSoftVdp2Reset,
    VIDSoftVdp2DrawStart,
    VIDSoftVdp2DrawEnd,
    VIDSoftVdp2DrawScreens,          // Próximamente: renderizar capas usando texturas
    VIDSoftVdp2SetResolution,
    VIDSoftVdp2SetPriorityNBG0,
    VIDSoftVdp2SetPriorityNBG1,
    VIDSoftVdp2SetPriorityNBG2,
    VIDSoftVdp2SetPriorityNBG3,
    VIDSoftVdp2SetPriorityRBG0,
    VIDSoftOnScreenDebugMessage,
    VIDSoftGetGlSize,
};
