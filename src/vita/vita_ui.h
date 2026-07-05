#ifndef VITA_UI_H
#define VITA_UI_H

// Inicializa la fuente de texto de Vita2D (llamar después de vita2d_init())
void init_vita_ui_font(void);

// Crea las carpetas necesarias en la PS Vita
void create_vita_directories(void);

// Muestra una interfaz gráfica para seleccionar un ROM
// Devuelve 1 si se seleccionó un archivo, 0 si se canceló
int select_rom(char *out_path, int max_len);

#endif
