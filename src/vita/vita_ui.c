#include "vita_ui.h"
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define ROMS_DIR "ux0:data/yabause/roms"
#define BIOS_DIR "ux0:data/yabause/bios"

static vita2d_pgf *pgf = NULL;

void init_vita_ui_font() {
    pgf = vita2d_load_default_pgf(); // Carga la fuente del sistema de la Vita
}

void create_vita_directories(void) {
    // Crear carpeta raíz de Yabause
    sceIoMkdir("ux0:data/yabause", 0777);
    // Crear carpeta para las BIOS de todas las regiones
    sceIoMkdir(BIOS_DIR, 0777);
    // Crear carpeta para los ROMs
    sceIoMkdir(ROMS_DIR, 0777);
}

int select_rom(char *out_path, int max_len) {
    DIR *d;
    struct dirent *dir;
    char files[100][256];
    int count = 0;
    int selected = 0;
    SceCtrlData pad, old_pad;
    memset(&old_pad, 0, sizeof(old_pad));

    // Leer los archivos de la carpeta de ROMs
    d = opendir(ROMS_DIR);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            // Ignorar "." y ".."
            if (dir->d_name[0] != '.') {
                strncpy(files[count], dir->d_name, 256);
                count++;
                if (count >= 100) break;
            }
        }
        closedir(d);
    }

    if (pgf == NULL) {
        init_vita_ui_font();
    }

    // Si no hay ROMs, mostrar mensaje y salir
    if (count == 0) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (pgf) {
            vita2d_pgf_draw_text(pgf, 10, 30, RGBA8(255, 0, 0, 255), 1.0f, "No se encontraron ROMs.");
            vita2d_pgf_draw_text(pgf, 10, 60, RGBA8(255, 255, 255, 255), 1.0f, "Copia tus juegos (.bin/.cue) a:");
            vita2d_pgf_draw_text(pgf, 10, 90, RGBA8(255, 255, 255, 255), 1.0f, "ux0:data/yabause/roms/");
            vita2d_pgf_draw_text(pgf, 10, 150, RGBA8(255, 255, 255, 255), 1.0f, "Presiona Circulo para salir.");
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
        
        while (1) {
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if (pad.buttons & SCE_CTRL_CIRCLE) break;
            sceKernelDelayThread(10000);
        }
        return 0;
    }

    // Bucle principal de la interfaz de selección
    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        
        // Detectar pulsaciones (no manteniendo presionado)
        if ((pad.buttons & SCE_CTRL_UP) && !(old_pad.buttons & SCE_CTRL_UP)) {
            selected--;
            if (selected < 0) selected = count - 1;
        } else if ((pad.buttons & SCE_CTRL_DOWN) && !(old_pad.buttons & SCE_CTRL_DOWN)) {
            selected++;
            if (selected >= count) selected = 0;
        } else if ((pad.buttons & SCE_CTRL_CROSS) && !(old_pad.buttons & SCE_CTRL_CROSS)) {
            snprintf(out_path, max_len, "%s/%s", ROMS_DIR, files[selected]);
            return 1; // Archivo seleccionado
        } else if ((pad.buttons & SCE_CTRL_CIRCLE) && !(old_pad.buttons & SCE_CTRL_CIRCLE)) {
            return 0; // Cancelado
        }
        old_pad = pad;

        // Dibujar la interfaz
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (pgf) {
            vita2d_pgf_draw_text(pgf, 10, 30, RGBA8(255, 255, 255, 255), 1.0f, "Selecciona un ROM (X = Jugar, O = Salir):");

            for (int i = 0; i < count; i++) {
                int y = 70 + i * 30;
                // Resaltar el seleccionado en amarillo, el resto en blanco
                unsigned int color = (i == selected) ? RGBA8(255, 255, 0, 255) : RGBA8(255, 255, 255, 255);
                vita2d_pgf_draw_text(pgf, 10, y, color, 1.0f, files[i]);
            }
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}
