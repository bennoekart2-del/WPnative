/* Host platform: SDL3 window, XFB presentation and up to four gamepads as Wii Remotes.
 *
 * Default mapping (Wii Party mostly holds the Remote sideways or points at the screen):
 *   A / Cross  -> A          B / Circle -> B        X -> 1        Y -> 2
 *   Start      -> +          Back       -> -        Guide -> Home
 *   D-pad      -> D-pad      Right stick -> pointer  Left stick -> tilt
 * Player 1 can also use keyboard + mouse.
 */
#include <SDL3/SDL.h>
#include <string.h>

#include "runtime.h"

/* Wii Remote core button bits as reported by WPAD (wiibrew: Wiimote#Buttons) */
enum {
    WPAD_LEFT = 0x0001, WPAD_RIGHT = 0x0002, WPAD_DOWN = 0x0004, WPAD_UP = 0x0008, WPAD_PLUS = 0x0010,
    WPAD_2 = 0x0100, WPAD_1 = 0x0200, WPAD_B = 0x0400, WPAD_A = 0x0800, WPAD_MINUS = 0x1000, WPAD_HOME = 0x8000,
};

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* xfb_tex;
static int xfb_w, xfb_h;
static SDL_Gamepad* pads[MAX_PLAYERS];
static HostRemote remotes[MAX_PLAYERS];

int platform_init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    if (!SDL_CreateWindowAndRenderer(title, width, height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("window failed: %s", SDL_GetError());
        return 0;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return 1;
}

static void assign_pads(void) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (pads[p] && !SDL_GamepadConnected(pads[p])) { SDL_CloseGamepad(pads[p]); pads[p] = NULL; }
    }
    for (int i = 0; ids && i < count; i++) {
        int taken = 0;
        for (int p = 0; p < MAX_PLAYERS; p++)
            if (pads[p] && SDL_GetGamepadID(pads[p]) == ids[i]) taken = 1;
        if (taken) continue;
        for (int p = 0; p < MAX_PLAYERS; p++)
            if (!pads[p]) { pads[p] = SDL_OpenGamepad(ids[i]); break; }
    }
    SDL_free(ids);
}

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return (v > -0.15f && v < 0.15f) ? 0.0f : v;
}

static void poll_input(void) {
    const bool* keys = SDL_GetKeyboardState(NULL);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        HostRemote* r = &remotes[p];
        SDL_Gamepad* g = pads[p];
        uint16_t b = 0;
        r->connected = g != NULL || p == 0;
        if (g) {
            static const struct { SDL_GamepadButton sdl; uint16_t wii; } map[] = {
                {SDL_GAMEPAD_BUTTON_SOUTH, WPAD_A}, {SDL_GAMEPAD_BUTTON_EAST, WPAD_B},
                {SDL_GAMEPAD_BUTTON_WEST, WPAD_1}, {SDL_GAMEPAD_BUTTON_NORTH, WPAD_2},
                {SDL_GAMEPAD_BUTTON_START, WPAD_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, WPAD_MINUS},
                {SDL_GAMEPAD_BUTTON_GUIDE, WPAD_HOME}, {SDL_GAMEPAD_BUTTON_DPAD_UP, WPAD_UP},
                {SDL_GAMEPAD_BUTTON_DPAD_DOWN, WPAD_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, WPAD_LEFT},
                {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, WPAD_RIGHT}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, WPAD_B},
            };
            for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
                if (SDL_GetGamepadButton(g, map[i].sdl)) b |= map[i].wii;
            r->ir_x = axis(g, SDL_GAMEPAD_AXIS_RIGHTX);
            r->ir_y = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
            r->accel[0] = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
            r->accel[1] = axis(g, SDL_GAMEPAD_AXIS_LEFTY);
            r->accel[2] = 1.0f;
        }
        if (p == 0) {
            if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE]) b |= WPAD_A;
            if (keys[SDL_SCANCODE_BACKSPACE]) b |= WPAD_B;
            if (keys[SDL_SCANCODE_1]) b |= WPAD_1;
            if (keys[SDL_SCANCODE_2]) b |= WPAD_2;
            if (keys[SDL_SCANCODE_EQUALS]) b |= WPAD_PLUS;
            if (keys[SDL_SCANCODE_MINUS]) b |= WPAD_MINUS;
            if (keys[SDL_SCANCODE_H]) b |= WPAD_HOME;
            if (keys[SDL_SCANCODE_UP]) b |= WPAD_UP;
            if (keys[SDL_SCANCODE_DOWN]) b |= WPAD_DOWN;
            if (keys[SDL_SCANCODE_LEFT]) b |= WPAD_LEFT;
            if (keys[SDL_SCANCODE_RIGHT]) b |= WPAD_RIGHT;
            if (!g) {
                float mx, my, w = 640, h = 480;
                SDL_GetMouseState(&mx, &my);
                SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
                r->ir_x = mx / w * 2 - 1;
                r->ir_y = my / h * 2 - 1;
                if (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) b |= WPAD_A;
            }
        }
        r->buttons = b;
    }
}

int platform_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) assign_pads();
    }
    poll_input();
    return 1;
}


/* The VI scans out a YUYV 4:2:2 framebuffer from guest memory. */
void platform_present_xfb(const uint8_t* yuyv, int width, int height, int pitch) {
    if (!xfb_tex || xfb_w != width || xfb_h != height) {
        if (xfb_tex) SDL_DestroyTexture(xfb_tex);
        xfb_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, width, height);
        xfb_w = width; xfb_h = height;
    }
    if (yuyv) SDL_UpdateTexture(xfb_tex, NULL, yuyv, pitch);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (yuyv) SDL_RenderTexture(renderer, xfb_tex, NULL, NULL);
    SDL_RenderPresent(renderer);
}

const HostRemote* platform_remote(int player) { return &remotes[player]; }

void platform_shutdown(void) {
    for (int p = 0; p < MAX_PLAYERS; p++) if (pads[p]) SDL_CloseGamepad(pads[p]);
    SDL_Quit();
}
