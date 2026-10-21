#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int boot_load(const char* disc_root, uint32_t* entry_out);

/* Host platform layer (platform_sdl.c): window, presentation, input. */
#define MAX_PLAYERS 4
typedef struct {
    int connected;
    uint16_t buttons;      /* Wii Remote button bits (WPAD_BUTTON_*), as the game reads them */
    float ir_x, ir_y;      /* pointer position, -1..1, driven by the right stick or mouse */
    float accel[3];        /* tilt, driven by the left stick */
} HostRemote;

int platform_init(const char* title, int width, int height);
int platform_pump(void);                /* returns 0 when the user closed the window */
void platform_present_xfb(const uint8_t* yuyv, int width, int height, int pitch); /* VI external framebuffer */
const HostRemote* platform_remote(int player);
void platform_shutdown(void);
void platform_set_fps(int fps);         /* shown in the title bar */

/* Hardware register handlers (hw.c) */
void hw_vi_write(uint32_t addr, uint32_t val, int size);
uint32_t hw_vi_read(uint32_t addr, int size);

#ifdef __cplusplus
}
#endif
