/* Host platform: SDL3 window, XFB presentation and up to four gamepads as Wii Remotes.
 *
 * Default mapping (Wii Party mostly holds the Remote sideways or points at the screen):
 *   A / Cross  -> A          B / Circle -> B        X -> 1        Y -> 2
 *   Start      -> +          Back       -> -        Guide -> Home
 *   D-pad      -> D-pad      Right stick -> pointer  Left stick -> tilt
 * One player per PC by default: the keyboard, the mouse and one gamepad all play player 1. With
 * WPR_LOCAL_PLAYERS=n, gamepads are players 2..n. A gamepad joins when one of its buttons is pressed.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_gl.h"

extern int g_gx_no_cull, g_gx_trace_request;  /* gx_render.c */

#include "netplay.h"
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
/* Gamepads Windows reports but that haven't pressed a button yet. Like syncing a real Remote, a pad
 * becomes a player only when a button is pressed on it, so a virtual or unused pad isn't a player. */
#define MAX_WAITING 8
static SDL_Gamepad* waiting_pads[MAX_WAITING];
static int pad_hold[MAX_PLAYERS];  /* the button that joined is still down: don't pass it on */
static HostRemote remotes[MAX_PLAYERS];
static int sideways;
static SDL_GLContext gl_context;  /* with OpenGL the GX renderer draws into the window itself */
static SDL_AtomicInt view[4];      /* where the picture sits in the window, in pixels */

int platform_gl_make_current(void) {
    if (!gl_context || !SDL_GL_MakeCurrent(window, gl_context)) return 0;
    if (!SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(1);
    return 1;
}

int platform_gl_window_size(int* w, int* h) { return SDL_GetWindowSizeInPixels(window, w, h); }
void platform_gl_swap(void) { SDL_GL_SwapWindow(window); }

void platform_set_view(int x, int y, int w, int h) {
    SDL_SetAtomicInt(&view[0], x); SDL_SetAtomicInt(&view[1], y);
    SDL_SetAtomicInt(&view[2], w); SDL_SetAtomicInt(&view[3], h);
}

/* 16:9 unless WPR_ASPECT=4:3; the console's aspect setting (nand_defaults.c) follows the same choice */
int platform_widescreen(void) {
    const char* a = getenv("WPR_ASPECT");
    return !(a && strcmp(a, "4:3") == 0);
}

/* How many times 640x528 the GPU draws: enough for the screen's height (1080p -> 3, 1440p -> 3,
 * 4K -> 5). WPR_SCALE=n overrides. */
int platform_render_scale(void) {
    const char* e = getenv("WPR_SCALE");
    int s = e ? atoi(e) : 0;
    if (s <= 0) {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        s = m ? (m->h + 479) / 480 : 2;
    }
    return s < 1 ? 1 : s > 6 ? 6 : s;
}

static void load_key_overrides(void);

int platform_init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    if (platform_widescreen()) width = height * 16 / 9;
    /* the GX renderer's GL context lives on the GPU thread and presents into this window itself */
    if (!getenv("WPR_RENDERER") || strcmp(getenv("WPR_RENDERER"), "soft") != 0) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (window) gl_context = SDL_GL_CreateContext(window);
        if (gl_context) SDL_GL_MakeCurrent(window, NULL);
        else {
            SDL_Log("no OpenGL 3.3 (%s): using the software renderer", SDL_GetError());
            if (window) SDL_DestroyWindow(window);
            window = NULL;
        }
    }
    if (!gl_context && !SDL_CreateWindowAndRenderer(title, width, height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("window failed: %s", SDL_GetError());
        return 0;
    }
    if (renderer) {
        SDL_SetRenderVSync(renderer, 1);
        SDL_SetRenderLogicalPresentation(renderer, platform_widescreen() ? 848 : 640, 480,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
    load_key_overrides();
    if (getenv("WPR_SIDEWAYS") && atoi(getenv("WPR_SIDEWAYS"))) sideways = 1;
    if (getenv("WPR_FULLSCREEN") && atoi(getenv("WPR_FULLSCREEN"))) SDL_SetWindowFullscreen(window, true);
    platform_set_fps(0);
    return 1;
}

static const char* pad_name(SDL_Gamepad* g) {
    const char* n = SDL_GetGamepadName(g);
    return n ? n : "gamepad";
}

static void assign_pads(void) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int p = 0; p < MAX_PLAYERS; p++)
        if (pads[p] && !SDL_GamepadConnected(pads[p])) {
            fprintf(stderr, "[input] player %d's gamepad is gone\n", p + 1);
            SDL_CloseGamepad(pads[p]);
            pads[p] = NULL;
        }
    for (int i = 0; i < MAX_WAITING; i++)
        if (waiting_pads[i] && !SDL_GamepadConnected(waiting_pads[i])) { SDL_CloseGamepad(waiting_pads[i]); waiting_pads[i] = NULL; }
    for (int i = 0; ids && i < count; i++) {
        int taken = 0;
        for (int p = 0; p < MAX_PLAYERS; p++)
            if (pads[p] && SDL_GetGamepadID(pads[p]) == ids[i]) taken = 1;
        for (int w = 0; w < MAX_WAITING; w++)
            if (waiting_pads[w] && SDL_GetGamepadID(waiting_pads[w]) == ids[i]) taken = 1;
        if (taken) continue;
        for (int w = 0; w < MAX_WAITING; w++)
            if (!waiting_pads[w]) {
                waiting_pads[w] = SDL_OpenGamepad(ids[i]);
                if (waiting_pads[w])
                    fprintf(stderr, "[input] found %s: press a button on it to play\n", pad_name(waiting_pads[w]));
                break;
            }
    }
    SDL_free(ids);
}

static int any_button(SDL_Gamepad* g) {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) return 1;
    return 0;
}

/* A waiting pad whose button is pressed becomes the next player. The keyboard is player 1, gamepads
 * are players 2..WPR_LOCAL_PLAYERS (default 1: one player per PC), and one more gamepad can play
 * player 1 together with the keyboard. Other pads are ignored. */
static void join_pads(void) {
    static int order[MAX_PLAYERS], norder = -1;
    if (norder < 0) {
        const char* e = getenv("WPR_LOCAL_PLAYERS");
        int n = e && *e ? atoi(e) : 1;
        if (n < 1) n = 1;
        if (n > MAX_PLAYERS) n = MAX_PLAYERS;
        norder = 0;
        for (int p = 1; p < n; p++) order[norder++] = p;
        order[norder++] = 0;
    }
    for (int w = 0; w < MAX_WAITING; w++) {
        if (!waiting_pads[w] || !any_button(waiting_pads[w])) continue;
        for (int j = 0; j < norder; j++)
            if (!pads[order[j]]) {
                pads[order[j]] = waiting_pads[w];
                pad_hold[order[j]] = 1;
                waiting_pads[w] = NULL;
                fprintf(stderr, "[input] player %d: %s\n", order[j] + 1, pad_name(pads[order[j]]));
                break;
            }
    }
}

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return (v > -0.15f && v < 0.15f) ? 0.0f : v;
}

/* The keyboard and mouse are player 1 (mouse pointer, left click A, right
 * click B; the arrow keys also move the pointer and press the D-pad); see the top of this file for gamepads.
 * F1 turns the direction keys for a Remote held sideways (as most minigames ask). */
typedef struct {
    SDL_Scancode up, down, left, right, a, b, one, two, plus, minus, home, shake, a2, b2, ab;
    SDL_Scancode up2, down2, left2, right2, shake2, plus2, minus2;
    SDL_Scancode grip, swing_up, swing_down, tilt_fwd, tilt_back, tilt_left, tilt_right;
} KeySet;
static KeySet p1_keys = {
    .up = SDL_SCANCODE_UP, .down = SDL_SCANCODE_DOWN, .left = SDL_SCANCODE_LEFT, .right = SDL_SCANCODE_RIGHT,
    .up2 = SDL_SCANCODE_W, .down2 = SDL_SCANCODE_S, .left2 = SDL_SCANCODE_A, .right2 = SDL_SCANCODE_D,
    .a = SDL_SCANCODE_RETURN, .a2 = SDL_SCANCODE_SPACE, .b = SDL_SCANCODE_BACKSPACE, .b2 = SDL_SCANCODE_RCTRL,
    .one = SDL_SCANCODE_1, .two = SDL_SCANCODE_2, .plus = SDL_SCANCODE_EQUALS, .plus2 = SDL_SCANCODE_KP_PLUS,
    .minus = SDL_SCANCODE_MINUS, .minus2 = SDL_SCANCODE_KP_MINUS, .home = SDL_SCANCODE_H,
    .shake = SDL_SCANCODE_LSHIFT, .shake2 = SDL_SCANCODE_RSHIFT, .grip = SDL_SCANCODE_F1,
    .swing_up = SDL_SCANCODE_T, .swing_down = SDL_SCANCODE_G, .tilt_fwd = SDL_SCANCODE_R, .tilt_back = SDL_SCANCODE_F,
    .tilt_left = SDL_SCANCODE_Q, .tilt_right = SDL_SCANCODE_E};
static uint64_t wheel_up_until, wheel_down_until;
#define MAX_G 3.4f  /* the most the Remote's accelerometer reports, in g */
static uint64_t swing_start[MAX_PLAYERS];
static float swing_dir[MAX_PLAYERS];  /* a mouse-wheel notch swings the Remote for a moment */
static uint64_t shake_start[MAX_PLAYERS];
static const KeySet no_keys = {SDL_SCANCODE_UNKNOWN};
static float key_ptr[MAX_PLAYERS][2];
static float tilt[MAX_PLAYERS][2];  /* current turn and lean angles, radians */
static uint64_t mouse_moved_ns, last_poll_ns;

/* The launcher rebinds player 1's keys with WPR_KEY_<NAME>=<SDL key name>, e.g. WPR_KEY_A=Space. */
static void load_key_overrides(void) {
    static const struct { const char* name; size_t off; } names[] = {
        {"UP", offsetof(KeySet, up)}, {"DOWN", offsetof(KeySet, down)}, {"LEFT", offsetof(KeySet, left)},
        {"RIGHT", offsetof(KeySet, right)}, {"A", offsetof(KeySet, a)}, {"B", offsetof(KeySet, b)},
        {"1", offsetof(KeySet, one)}, {"2", offsetof(KeySet, two)}, {"PLUS", offsetof(KeySet, plus)},
        {"MINUS", offsetof(KeySet, minus)}, {"HOME", offsetof(KeySet, home)}, {"SHAKE", offsetof(KeySet, shake)},
        {"A2", offsetof(KeySet, a2)}, {"B2", offsetof(KeySet, b2)}, {"AB", offsetof(KeySet, ab)},
        {"UP2", offsetof(KeySet, up2)}, {"DOWN2", offsetof(KeySet, down2)}, {"LEFT2", offsetof(KeySet, left2)},
        {"RIGHT2", offsetof(KeySet, right2)}, {"SHAKE2", offsetof(KeySet, shake2)}, {"GRIP", offsetof(KeySet, grip)},
        {"SWINGUP", offsetof(KeySet, swing_up)}, {"SWINGDOWN", offsetof(KeySet, swing_down)},
        {"TILTFWD", offsetof(KeySet, tilt_fwd)}, {"TILTBACK", offsetof(KeySet, tilt_back)},
        {"TILTLEFT", offsetof(KeySet, tilt_left)}, {"TILTRIGHT", offsetof(KeySet, tilt_right)},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char var[32];
        SDL_snprintf(var, sizeof var, "WPR_KEY_%s", names[i].name);
        const char* v = getenv(var);
        if (!v || !*v) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(v);
        if (sc == SDL_SCANCODE_UNKNOWN) { fprintf(stderr, "[keys] unknown key '%s' for %s\n", v, var); continue; }
        *(SDL_Scancode*)((char*)&p1_keys + names[i].off) = sc;
    }
}

static int down(const bool* keys, SDL_Scancode k) { return k != SDL_SCANCODE_UNKNOWN && keys[k]; }

static void poll_input(void) {
    const bool* keys = SDL_GetKeyboardState(NULL);
    uint64_t now = SDL_GetTicksNS();
    float dt = last_poll_ns ? (float)(now - last_poll_ns) / 1e9f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_poll_ns = now;
    join_pads();
    for (int p = 0; p < MAX_PLAYERS; p++) {
        HostRemote* r = &remotes[p];
        SDL_Gamepad* g = pads[p];
        const KeySet* k = p == 0 ? &p1_keys : &no_keys;
        uint16_t b = 0;
        int shake = 0;
        r->connected = p == 0 || g;
        if (g && pad_hold[p]) {
            if (any_button(g)) g = NULL; else pad_hold[p] = 0;
        }
        float turn = 0.0f, lean = 0.0f;  /* wanted tilt, -1..1: turn left/right, lean forward/back */
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
            turn = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
            lean = -axis(g, SDL_GAMEPAD_AXIS_LEFTY);
        }
        /* keyboard directions, turned for a sideways Remote (D-pad on the left: screen up = Right) */
        int ku = down(keys, k->up), kd = down(keys, k->down), kl = down(keys, k->left), kr = down(keys, k->right);
        int wu = down(keys, k->up2), wd = down(keys, k->down2), wl = down(keys, k->left2), wr = down(keys, k->right2);
        if (sideways) {
            if (wu) b |= WPAD_RIGHT;
            if (wd) b |= WPAD_LEFT;
            if (wl) b |= WPAD_UP;
            if (wr) b |= WPAD_DOWN;
        } else {
            if (wu) b |= WPAD_UP;
            if (wd) b |= WPAD_DOWN;
            if (wl) b |= WPAD_LEFT;
            if (wr) b |= WPAD_RIGHT;
        }
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
        if (down(keys, k->plus) || down(keys, k->plus2)) b |= WPAD_PLUS;
        if (down(keys, k->minus) || down(keys, k->minus2)) b |= WPAD_MINUS;
        if (down(keys, k->home)) b |= WPAD_HOME;
        if (down(keys, k->shake) || down(keys, k->shake2)) shake = 1;
        if (p == 0 && (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MMASK)) shake = 1;
        /* tilt forward / back (R / F), swing up / down (T / G or the mouse wheel) */
        if (down(keys, k->tilt_left)) turn = -1.0f;   /* Q / E */
        if (down(keys, k->tilt_right)) turn = 1.0f;
        if (down(keys, k->tilt_fwd)) lean = 1.0f;     /* R / F */
        if (down(keys, k->tilt_back)) lean = -1.0f;
        /* The Remote turns smoothly towards the wanted angle and back to level when let go, like
         * a hand would: up to 90 degrees when pointing at the screen (some games want it pointing
         * at the floor, or twisted a quarter turn), 60 held sideways. acc is the gravity reaction in
         * KPAD axes: x across the face, y out of the buttons (-1 when level), z along the Remote
         * (+1 pointing at the floor: the game's swing check waits for z > 1.7, the pull of a swing
         * on the Remote's tip). Held upright, leaning tips the Remote down onto z and turning rolls
         * it around its length onto x; held sideways, its length lies left-right, so turning like a
         * steering wheel moves gravity onto z and leaning moves it onto x. */
        float* ang = tilt[p];
        float step = 4.0f * dt, max = sideways ? 1.05f : 1.5708f;
        float want[2] = {turn * max, lean * max};
        for (int i = 0; i < 2; i++) {
            float d = want[i] - ang[i];
            ang[i] += d > step ? step : d < -step ? -step : d;
        }
        float a_turn = ang[0], a_lean = ang[1];
        if (sideways) {
            r->accel[0] = sinf(a_lean) * cosf(a_turn);
            r->accel[2] = -sinf(a_turn);
            r->accel[1] = -cosf(a_lean) * cosf(a_turn);
        } else {
            r->accel[0] = sinf(a_turn) * cosf(a_lean);
            r->accel[2] = sinf(a_lean);
            r->accel[1] = -cosf(a_turn) * cosf(a_lean);
        }
        /* A swing is a flick of the wrist: the Remote (15 cm out) turns 70 degrees up or down in
         * 0.15 s and comes back in 0.33 s. What it reads is the push of turning (y), the pull outwards
         * (z, which the game's swing check waits for) and gravity as the Remote turns. A held key or
         * more wheel notches swing again every 0.5 s. */
        int su = down(keys, k->swing_up) || (p == 0 && now < wheel_up_until);
        int sd = down(keys, k->swing_down) || (p == 0 && now < wheel_down_until);
        if ((su || sd) && now - swing_start[p] > 500000000ull) {
            swing_start[p] = now;
            swing_dir[p] = su ? 1.0f : -1.0f;
        }
        float st = (float)(now - swing_start[p]) / 1e9f;
        if (swing_start[p] && st < 0.48f) {
            const float out = 0.15f / 9.81f, go = 0.15f, back = 0.33f, full = 1.2f * swing_dir[p];
            float u = st < go ? st / go : (st - go) / back, T = st < go ? go : back, sg = st < go ? 1.0f : -1.0f;
            if (u > 1) u = 1;
            float pos = u * u * u * (10 - 15 * u + 6 * u * u);   /* smooth start and stop */
            float th = full * (st < go ? pos : 1 - pos);
            float w = sg * full / T * 30 * u * u * (1 - u) * (1 - u);
            float al = sg * full / (T * T) * 60 * u * (1 - u) * (1 - 2 * u);
            r->accel[1] += -out * al - cosf(th) + 1.0f;
            r->accel[2] += out * w * w - sinf(th);
        }
        if (shake) {
            /* a hard shake, about 7 times a second, on every axis (the game counts the swings in it) */
            if (!shake_start[p]) shake_start[p] = now;
            float v = 3.2f * sinf(2 * 3.14159f * 7.0f * (float)(now - shake_start[p]) / 1e9f);
            for (int i = 0; i < 3; i++) r->accel[i] += v;
        } else {
            shake_start[p] = 0;
        }
        for (int i = 0; i < 3; i++) r->accel[i] = fmaxf(-MAX_G, fminf(MAX_G, r->accel[i]));

        /* pointer: mouse (player 1, once it moves), right stick, or the direction keys */
        float* kp = key_ptr[p];
        float sp = 2.2f * dt;  /* screen widths (2.0) per second */
        int mouse_active = p == 0 && mouse_moved_ns && now - mouse_moved_ns < 3000000000ull;
        if (!sideways && !mouse_active) {
            kp[0] += (kr - kl) * sp;
            kp[1] += (kd - ku) * sp;
        }
        if (g) {
            float sx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX), sy = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
            /* fast when pushed all the way, still precise when pushed a little */
            kp[0] += sx * fabsf(sx) * sp * 1.6f;
            kp[1] += sy * fabsf(sy) * sp * 1.6f;
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
            if (mouse_active && renderer) {
                SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
                r->ir_x = kp[0] = mx / (platform_widescreen() ? 848.0f : 640.0f) * 2 - 1;
                r->ir_y = kp[1] = my / 480.0f * 2 - 1;
            } else if (mouse_active && SDL_GetAtomicInt(&view[2]) > 0) {
                float d = SDL_GetWindowPixelDensity(window);
                r->ir_x = kp[0] = (mx * d - (float)SDL_GetAtomicInt(&view[0])) / (float)SDL_GetAtomicInt(&view[2]) * 2 - 1;
                r->ir_y = kp[1] = (my * d - (float)SDL_GetAtomicInt(&view[1])) / (float)SDL_GetAtomicInt(&view[3]) * 2 - 1;
            }
            if (mb & SDL_BUTTON_LMASK) b |= WPAD_A;
            if (mb & SDL_BUTTON_RMASK) b |= WPAD_B;
        }
        r->buttons = b;
    }
}

/* Title bar: frame rate and how the keyboard directions are turned. fps < 0 keeps the last one. */
void platform_set_fps(int fps) {
    static int last;
    if (fps >= 0) last = fps;
    char t[360];
    const char* net = net_status();
    SDL_snprintf(t, sizeof t, "Wii Party  |  %s%s%d fps  |  %s (F1 to change)  |  F11 full screen", net ? net : "",
                 net ? "  |  " : "", last, sideways ? "Remotes held sideways" : "Remotes pointing at the screen");
    SDL_SetWindowTitle(window, t);
}

int platform_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) assign_pads();
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_moved_ns = SDL_GetTicksNS();
        if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0) {
            uint64_t until = SDL_GetTicksNS() + 150000000ull;
            if (e.wheel.y > 0) wheel_up_until = until; else wheel_down_until = until;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.scancode == SDL_SCANCODE_F1 || e.key.scancode == p1_keys.grip) {
                sideways = !sideways;
                platform_set_fps(-1);
            }
            if (e.key.scancode == SDL_SCANCODE_F8) {  /* draw hidden faces too, to check a graphics bug */
                g_gx_no_cull = !g_gx_no_cull;
                fprintf(stderr, "[gx] face culling %s (F8)\n", g_gx_no_cull ? "off" : "on");
            }
            if (e.key.scancode == SDL_SCANCODE_F9) g_gx_trace_request = 1;
            if (e.key.scancode == SDL_SCANCODE_F11)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        }
    }
    poll_input();
    return 1;
}


/* The VI scans out a YUYV 4:2:2 framebuffer from guest memory. */
void platform_present_xfb(const uint8_t* yuyv, int width, int height, int pitch) {
    if (!renderer) {
        if (gx_gl_state() != 0) { SDL_Delay(2); return; }  /* the GPU thread presents (gx_gl_present) */
        /* the GPU renderer could not start after all: present the software renderer's frames */
        renderer = SDL_CreateRenderer(window, NULL);
        if (!renderer) { SDL_Delay(2); return; }
        SDL_SetRenderVSync(renderer, 1);
        SDL_SetRenderLogicalPresentation(renderer, platform_widescreen() ? 848 : 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
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
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}
