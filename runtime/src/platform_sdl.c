/* Host platform: SDL3 window, XFB presentation and up to four gamepads as Wii Remotes.
 *
 * Default mapping (Wii Party mostly holds the Remote sideways or points at the screen):
 *   A / Cross  -> A          B / Circle -> B        X -> 1        Y -> 2
 *   Start      -> +          Back       -> -        Guide -> Home
 *   D-pad      -> D-pad      Right stick -> pointer  Left stick -> tilt
 * One player per device: the keyboard and mouse are player 1, each gamepad is the next player (2-4).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_gl.h"

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
static SDL_Window* gl_window;
static SDL_GLContext gl_context;

int platform_gl_make_current(void) {
    return gl_context && SDL_GL_MakeCurrent(gl_window, gl_context);
}

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
    platform_set_fps(0);
    /* the GX renderer's GL context lives on the GPU thread; it draws offscreen, so its window stays hidden */
    if (!getenv("WPR_RENDERER") || strcmp(getenv("WPR_RENDERER"), "soft") != 0) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        gl_window = SDL_CreateWindow("gx", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (gl_window) gl_context = SDL_GL_CreateContext(gl_window);
        if (gl_context) SDL_GL_MakeCurrent(gl_window, NULL);
        else SDL_Log("no OpenGL 3.3 (%s): using the software renderer", SDL_GetError());
    }
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
        /* the keyboard and mouse are player 1, so gamepads fill players 2-4 first */
        static const int order[MAX_PLAYERS] = {1, 2, 3, 0};
        for (int j = 0; j < MAX_PLAYERS; j++)
            if (!pads[order[j]]) { pads[order[j]] = SDL_OpenGamepad(ids[i]); break; }
    }
    SDL_free(ids);
}

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return (v > -0.15f && v < 0.15f) ? 0.0f : v;
}

/* One player per device: the keyboard and mouse are player 1 (mouse pointer, left click A, right
 * click B; the arrow keys also move the pointer and press the D-pad), each gamepad is one more player.
 * F1 turns the arrow keys for a Remote held sideways (as most minigames ask). */
typedef struct {
    SDL_Scancode up, down, left, right, a, b, one, two, plus, minus, home, shake, a2, b2, ab;
} KeySet;
static const KeySet p1_keys = {
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_EQUALS, SDL_SCANCODE_MINUS,
    SDL_SCANCODE_H, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_RCTRL, SDL_SCANCODE_TAB};
static const KeySet no_keys = {SDL_SCANCODE_UNKNOWN};
static int sideways;
static float key_ptr[MAX_PLAYERS][2];
static uint64_t mouse_moved_ns, last_poll_ns;

static int down(const bool* keys, SDL_Scancode k) { return k != SDL_SCANCODE_UNKNOWN && keys[k]; }

static void poll_input(void) {
    const bool* keys = SDL_GetKeyboardState(NULL);
    uint64_t now = SDL_GetTicksNS();
    float dt = last_poll_ns ? (float)(now - last_poll_ns) / 1e9f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_poll_ns = now;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        HostRemote* r = &remotes[p];
        SDL_Gamepad* g = pads[p];
        const KeySet* k = p == 0 ? &p1_keys : &no_keys;
        uint16_t b = 0;
        int shake = 0;
        r->connected = p == 0 || g;
        r->accel[0] = r->accel[1] = 0.0f;
        r->accel[2] = 1.0f;
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
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) shake = 1;
            r->accel[0] = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
            r->accel[1] = axis(g, SDL_GAMEPAD_AXIS_LEFTY);
        }
        /* keyboard directions, turned for a sideways Remote (D-pad on the left: screen up = Right) */
        int ku = down(keys, k->up), kd = down(keys, k->down), kl = down(keys, k->left), kr = down(keys, k->right);
        if (sideways) {
            if (ku) b |= WPAD_RIGHT;
            if (kd) b |= WPAD_LEFT;
            if (kl) b |= WPAD_UP;
            if (kr) b |= WPAD_DOWN;
        } else {
            if (ku) b |= WPAD_UP;
            if (kd) b |= WPAD_DOWN;
            if (kl) b |= WPAD_LEFT;
            if (kr) b |= WPAD_RIGHT;
        }
        if (down(keys, k->a) || down(keys, k->a2)) b |= WPAD_A;
        if (down(keys, k->b) || down(keys, k->b2)) b |= WPAD_B;
        if (down(keys, k->ab)) b |= WPAD_A | WPAD_B;
        if (down(keys, k->one)) b |= WPAD_1;
        if (down(keys, k->two)) b |= WPAD_2;
        if (down(keys, k->plus)) b |= WPAD_PLUS;
        if (down(keys, k->minus)) b |= WPAD_MINUS;
        if (down(keys, k->home)) b |= WPAD_HOME;
        if (down(keys, k->shake)) shake = 1;
        if (shake) {  /* a hard back-and-forth swing */
            float ph = (float)(now % 125000000ull) / 125000000.0f;
            r->accel[0] = ph < 0.5f ? 3.0f : -3.0f;
            r->accel[1] = ph < 0.5f ? -2.5f : 2.5f;
            r->accel[2] = ph < 0.5f ? 2.0f : -1.0f;
        }

        /* pointer: mouse (player 1, once it moves), right stick, or the direction keys */
        float* kp = key_ptr[p];
        float sp = 1.3f * dt;
        int mouse_active = p == 0 && mouse_moved_ns && now - mouse_moved_ns < 3000000000ull;
        if (!sideways && !mouse_active) {
            kp[0] += (kr - kl) * sp;
            kp[1] += (kd - ku) * sp;
        }
        if (g) {
            float sx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX), sy = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
            kp[0] += sx * sp * 1.5f;
            kp[1] += sy * sp * 1.5f;
        }
        if (kp[0] < -1) kp[0] = -1;
        if (kp[0] > 1) kp[0] = 1;
        if (kp[1] < -1) kp[1] = -1;
        if (kp[1] > 1) kp[1] = 1;
        r->ir_x = kp[0];
        r->ir_y = kp[1];
        if (p == 0) {
            float mx, my;
            SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
            if (mouse_active) {
                SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
                r->ir_x = kp[0] = mx / 640.0f * 2 - 1;
                r->ir_y = kp[1] = my / 480.0f * 2 - 1;
            }
            if (mb & SDL_BUTTON_LMASK) b |= WPAD_A;
            if (mb & SDL_BUTTON_RMASK) b |= WPAD_B;
            if (mb & SDL_BUTTON_MMASK) b |= WPAD_A | WPAD_B;
        }
        r->buttons = b;
    }
}

/* Title bar: frame rate and how the keyboard directions are turned. fps < 0 keeps the last one. */
void platform_set_fps(int fps) {
    static int last;
    if (fps >= 0) last = fps;
    char t[160];
    SDL_snprintf(t, sizeof t, "Wii Party  |  %d fps  |  %s (F1 to change)  |  F11 full screen", last,
                 sideways ? "Remotes held sideways" : "Remotes pointing at the screen");
    SDL_SetWindowTitle(window, t);
}

int platform_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) assign_pads();
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_moved_ns = SDL_GetTicksNS();
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.scancode == SDL_SCANCODE_F1) {
                sideways = !sideways;
                platform_set_fps(-1);
            }
            if (e.key.scancode == SDL_SCANCODE_F11)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        }
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

/* The guest, GPU and timer threads are still running game code here, so SDL is not torn down
 * under them (SDL_Quit would pull the GL context and audio device away mid-frame): the window
 * goes, then the process ends. Saves are already on disk, NAND writes are synchronous. */
void platform_shutdown(void) {
    for (int p = 0; p < MAX_PLAYERS; p++) if (pads[p]) SDL_CloseGamepad(pads[p]);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}
