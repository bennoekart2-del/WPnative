/* Netplay. Every player runs the whole game in deterministic mode (det.c); only controller input
 * travels, so the game must already be on every PC (each player's own disc and build).
 *
 * The host is a star: each friend sends the host their input for frame f + delay; once the host
 * has every player's input for a frame it sends the complete frame to everyone, and every game
 * waits at each VI retrace until that frame's input is there (net_frame). All games then read
 * exactly the same input on exactly the same frame. Before the game boots, the host sends its save
 * data (the NAND) and the settings that change the game, so everyone starts from the same state.
 * Each frame every player also hashes a slice of guest memory; the host compares them and tells
 * everyone if the games ever drift apart.
 *
 * Messages are [u8 type][u32 length][payload] over TCP, little endian.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesocket close
#endif
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netplay.h"
#include "ppc_context.h"

#define NET_MAGIC 0x504E5057u  /* "WPNP" */
#define NET_VERSION 1
#define DEFAULT_PORT 47123
#define MAX_PEERS 4
#define RING 512
#define REMOTE_BYTES 24

enum { M_HELLO = 1, M_REJECT, M_PING, M_PONG, M_FILE, M_START, M_INPUT, M_FRAME, M_HASH, M_DESYNC, M_BYE };

typedef struct {
    sock_t s;
    SDL_Mutex* send_lock;
    int alive;
    int nlocal;
    char name[32];
    uint64_t hash[RING];      /* host: this player's memory hash per frame, tagged below */
    uint32_t hash_tag[RING];  /* frame + 1 */
} Peer;

static int mode;               /* 0 off, 1 host, 2 joined */
static int me;                 /* our peer index; the host is 0 */
static int delay = 4;          /* frames between sampling input and using it */
static Peer peers[MAX_PEERS];  /* host: peers[1..] are friends; joiner: peers[0] is the host */
static int npeers = 1, nplayers;
static int owner[MAX_PLAYERS] = {-1, -1, -1, -1};  /* which peer plays each player slot */
static int nlocal_devs, local_devs[MAX_PLAYERS];     /* our devices, in slot order */

static SDL_Mutex* lock;
static SDL_Condition* cond;
static HostRemote frames[RING][MAX_PLAYERS];
static uint32_t frame_tag[RING];                     /* frame + 1 once complete */
static HostRemote pend[RING][MAX_PLAYERS];           /* host: inputs gathered so far */
static uint32_t pend_tag[RING][MAX_PLAYERS];
static uint32_t host_cur, host_newest;               /* host: frame being played, newest sampled */
static uint64_t my_hash[RING];
static uint32_t my_hash_tag[RING];
static HostRemote cur[MAX_PLAYERS];
static char status[160];
static int desynced, host_gone;

int net_active(void) { return mode != 0; }

static void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v && *v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* lock held (or before any thread starts) */
static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[net] %s\n", status);
}

const char* net_status(void) {
    static char copy[160];
    if (!lock) return NULL;
    SDL_LockMutex(lock);
    memcpy(copy, status, sizeof copy);
    SDL_UnlockMutex(lock);
    return copy[0] ? copy : NULL;
}

/* ------------------------------------------------------------------ sockets */
static void sock_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

static void sock_blocking(sock_t s, int on) {
#ifdef _WIN32
    u_long nb = !on;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, on ? fcntl(s, F_GETFL) & ~O_NONBLOCK : fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
}

static int send_all(sock_t s, const void* p, size_t n) {
    const char* c = p;
    while (n) {
        int k = send(s, c, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
        if (k <= 0) return 0;
        c += k;
        n -= (size_t)k;
    }
    return 1;
}

static int recv_all(sock_t s, void* p, size_t n) {
    char* c = p;
    while (n) {
        int k = recv(s, c, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
        if (k <= 0) return 0;
        c += k;
        n -= (size_t)k;
    }
    return 1;
}

static int send_msg(Peer* p, int type, const void* data, uint32_t len) {
    uint8_t h[5] = {(uint8_t)type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    SDL_LockMutex(p->send_lock);
    int ok = p->alive && send_all(p->s, h, 5) && (!len || send_all(p->s, data, len));
    SDL_UnlockMutex(p->send_lock);
    return ok;
}

/* A whole message; *data is malloc'd and zero-terminated (free it). */
static int recv_msg(sock_t s, int* type, uint8_t** data, uint32_t* len) {
    uint8_t h[5];
    if (!recv_all(s, h, 5)) return 0;
    *type = h[0];
    *len = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 | (uint32_t)h[4] << 24;
    if (*len > (64u << 20)) return 0;
    *data = malloc(*len + 1);
    if (!*data) return 0;
    (*data)[*len] = 0;
    if (*len && !recv_all(s, *data, *len)) { free(*data); return 0; }
    return 1;
}

/* Waits until s is readable (or, with write, connected), keeping the window alive meanwhile.
 * Returns 1 ready, 0 timeout, -1 the window was closed. */
static int wait_sock(sock_t s, int write, int ms) {
    uint64_t end = SDL_GetTicks() + (uint64_t)ms;
    for (;;) {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        struct timeval tv = {0, 50000};
        if (select((int)s + 1, write ? NULL : &fs, write ? &fs : NULL, NULL, &tv) > 0) return 1;
        if (!platform_pump()) return -1;
        platform_set_fps(-1);
        if (SDL_GetTicks() >= end) return 0;
    }
}

/* ------------------------------------------------------------------ input */
static void put_remote(uint8_t* o, const HostRemote* r) {
    o[0] = (uint8_t)(r->connected != 0);
    o[1] = 0;
    o[2] = (uint8_t)r->buttons;
    o[3] = (uint8_t)(r->buttons >> 8);
    memcpy(o + 4, &r->ir_x, 4);
    memcpy(o + 8, &r->ir_y, 4);
    memcpy(o + 12, r->accel, 12);
}

static void get_remote(const uint8_t* o, HostRemote* r) {
    r->connected = o[0];
    r->buttons = (uint16_t)(o[2] | o[3] << 8);
    memcpy(&r->ir_x, o + 4, 4);
    memcpy(&r->ir_y, o + 8, 4);
    memcpy(r->accel, o + 12, 12);
}

static HostRemote neutral(int slot) {
    HostRemote r;
    memset(&r, 0, sizeof r);
    r.connected = owner[slot] >= 0;
    r.accel[1] = -1.0f;
    return r;
}

const HostRemote* net_remote(int player) { return &cur[player]; }

void hle_input_sample(int dev, uint32_t count, HostRemote* out);  /* hle_input.c */

/* host, lock held: a frame is complete once every player slot has its input (a friend who left
 * plays on with no buttons pressed) */
static void try_complete(uint32_t f) {
    int i = (int)(f % RING);
    if (frame_tag[i] == f + 1) return;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (owner[s] < 0 || pend_tag[i][s] == f + 1) continue;
        if (owner[s] > 0 && !peers[owner[s]].alive) { pend[i][s] = neutral(s); pend_tag[i][s] = f + 1; continue; }
        return;
    }
    for (int s = 0; s < MAX_PLAYERS; s++) frames[i][s] = owner[s] < 0 ? neutral(s) : pend[i][s];
    frame_tag[i] = f + 1;
    uint8_t msg[4 + MAX_PLAYERS * REMOTE_BYTES];
    memcpy(msg, &f, 4);
    for (int s = 0; s < MAX_PLAYERS; s++) put_remote(msg + 4 + s * REMOTE_BYTES, &frames[i][s]);
    for (int p = 1; p < npeers; p++) if (peers[p].alive) send_msg(&peers[p], M_FRAME, msg, sizeof msg);
    SDL_BroadcastCondition(cond);
}

/* host, lock held */
static void compare_hashes(uint32_t f) {
    int i = (int)(f % RING);
    if (desynced || my_hash_tag[i] != f + 1) return;
    for (int p = 1; p < npeers; p++) {
        if (peers[p].hash_tag[i] != f + 1 || peers[p].hash[i] == my_hash[i]) continue;
        desynced = 1;
        say("Online: OUT OF SYNC with %s at frame %u - please restart", peers[p].name, f);
        for (int q = 1; q < npeers; q++) if (peers[q].alive) send_msg(&peers[q], M_DESYNC, &f, 4);
        return;
    }
}

static int SDLCALL host_recv(void* arg) {
    int p = (int)(intptr_t)arg;
    Peer* pe = &peers[p];
    int type;
    uint8_t* d;
    uint32_t len;
    while (recv_msg(pe->s, &type, &d, &len)) {
        if (type == M_PING) { send_msg(pe, M_PONG, d, len); free(d); continue; }
        SDL_LockMutex(lock);
        if (type == M_INPUT && len >= 5) {
            uint32_t f;
            memcpy(&f, d, 4);
            int n = d[4], k = 0;
            for (int s = 0; s < MAX_PLAYERS && k < n && 5 + (uint32_t)(k + 1) * REMOTE_BYTES <= len; s++)
                if (owner[s] == p) {
                    get_remote(d + 5 + k * REMOTE_BYTES, &pend[f % RING][s]);
                    pend_tag[f % RING][s] = f + 1;
                    k++;
                }
            try_complete(f);
        } else if (type == M_HASH && len >= 12) {
            uint32_t f;
            memcpy(&f, d, 4);
            memcpy(&pe->hash[f % RING], d + 4, 8);
            pe->hash_tag[f % RING] = f + 1;
            compare_hashes(f);
        }
        SDL_UnlockMutex(lock);
        free(d);
    }
    SDL_LockMutex(lock);
    pe->alive = 0;
    say("Online: %s left the game", pe->name);
    for (uint32_t f = host_cur; f <= host_newest; f++) try_complete(f);
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(lock);
    return 0;
}

static int SDLCALL client_recv(void* arg) {
    (void)arg;
    Peer* pe = &peers[0];
    int type;
    uint8_t* d;
    uint32_t len;
    while (recv_msg(pe->s, &type, &d, &len)) {
        if (type == M_FRAME && len >= 4 + MAX_PLAYERS * REMOTE_BYTES) {
            uint32_t f;
            memcpy(&f, d, 4);
            SDL_LockMutex(lock);
            for (int s = 0; s < MAX_PLAYERS; s++) get_remote(d + 4 + s * REMOTE_BYTES, &frames[f % RING][s]);
            frame_tag[f % RING] = f + 1;
            SDL_BroadcastCondition(cond);
            SDL_UnlockMutex(lock);
        } else if (type == M_DESYNC && len >= 4) {
            uint32_t f;
            memcpy(&f, d, 4);
            SDL_LockMutex(lock);
            desynced = 1;
            say("Online: OUT OF SYNC at frame %u - please restart", f);
            SDL_UnlockMutex(lock);
        } else if (type == M_PING) {
            send_msg(pe, M_PONG, d, len);
        }
        free(d);
    }
    SDL_LockMutex(lock);
    pe->alive = 0;
    host_gone = 1;
    say("Online: the host left the game - close this window");
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(lock);
    return 0;
}

void net_frame(uint64_t frame) {
    uint32_t f = (uint32_t)frame, g = f + (uint32_t)delay;
    HostRemote mine[MAX_PLAYERS];
    for (int k = 0; k < nlocal_devs; k++) hle_input_sample(local_devs[k], f, &mine[k]);
    if (mode == 1) {
        SDL_LockMutex(lock);
        host_cur = f;
        host_newest = g;
        for (int s = 0, k = 0; s < MAX_PLAYERS; s++)
            if (owner[s] == 0) { pend[g % RING][s] = mine[k++]; pend_tag[g % RING][s] = g + 1; }
        try_complete(g);
        SDL_UnlockMutex(lock);
    } else {
        uint8_t msg[5 + MAX_PLAYERS * REMOTE_BYTES];
        memcpy(msg, &g, 4);
        msg[4] = (uint8_t)nlocal_devs;
        for (int k = 0; k < nlocal_devs; k++) put_remote(msg + 5 + k * REMOTE_BYTES, &mine[k]);
        send_msg(&peers[0], M_INPUT, msg, 5 + (uint32_t)nlocal_devs * REMOTE_BYTES);
    }
    SDL_LockMutex(lock);
    uint64_t t0 = SDL_GetTicks();
    int waited = 0;
    while (frame_tag[f % RING] != f + 1) {
        SDL_WaitConditionTimeout(cond, lock, 100);
        if (!waited && SDL_GetTicks() - t0 > 1000 && !desynced && !host_gone) {
            waited = 1;
            if (mode == 2) say("Online: waiting for the other players...");
            else {
                const char* who = "a friend";
                for (int s = 0; s < MAX_PLAYERS; s++)
                    if (owner[s] > 0 && pend_tag[f % RING][s] != f + 1) who = peers[owner[s]].name;
                say("Online: waiting for %s...", who);
            }
        }
    }
    if (waited && !desynced && !host_gone) say("Online: %d players", nplayers);
    memcpy(cur, frames[f % RING], sizeof cur);
    SDL_UnlockMutex(lock);
}

void net_hash(uint64_t frame, uint64_t hash) {
    uint32_t f = (uint32_t)frame;
    if (mode == 1) {
        SDL_LockMutex(lock);
        my_hash[f % RING] = hash;
        my_hash_tag[f % RING] = f + 1;
        compare_hashes(f);
        SDL_UnlockMutex(lock);
    } else {
        uint8_t msg[12];
        memcpy(msg, &f, 4);
        memcpy(msg + 4, &hash, 8);
        send_msg(&peers[0], M_HASH, msg, 12);
    }
}

/* ------------------------------------------------------------------ session setup */
extern const PPCFuncEntry g_recomp_funcs[];
extern const unsigned g_recomp_func_count;

static uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* The same disc gives the same recompiled functions, whoever built it and when. */
static uint32_t build_id(void) {
    uint32_t h = fnv(2166136261u, &g_recomp_func_count, 4);
    for (unsigned i = 0; i < g_recomp_func_count; i++) h = fnv(h, &g_recomp_funcs[i].addr, 4);
    return h;
}

static uint32_t disc_id(const char* root) {
    char path[1200];
    uint32_t h = 2166136261u;
    size_t n = 0;
    snprintf(path, sizeof path, "%s/sys/boot.bin", root);
    void* d = SDL_LoadFile(path, &n);
    if (d) { h = fnv(h, d, n > 8 ? 8 : n); SDL_free(d); }
    snprintf(path, sizeof path, "%s/sys/main.dol", root);
    d = SDL_LoadFile(path, &n);
    if (d) { h = fnv(h, d, n); SDL_free(d); }
    return h;
}

static void nand_root_of(const char* disc_root, char* out, size_t n) {
    const char* env = getenv("WPR_NAND");
    if (env) snprintf(out, n, "%s", env);
    else snprintf(out, n, "%s/../nand", disc_root);
}

/* Settings that change what the game does, not just how it looks: everyone uses the host's. */
static const char* shared_env[] = {"WPR_ASPECT", "WPR_LANGUAGE"};

static void fail(const char* msg) {
    SDL_LockMutex(lock);
    say("Online: %s", msg);
    SDL_UnlockMutex(lock);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wii Party online", msg, NULL);
}

typedef struct { uint8_t* buf; uint32_t len, cap; } Buf;
static void buf_put(Buf* b, const void* p, uint32_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 256; b->buf = realloc(b->buf, b->cap); }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void send_nand(Peer* pe, const char* root) {
    int count = 0;
    char** names = SDL_GlobDirectory(root, NULL, 0, &count);
    for (int i = 0; names && i < count; i++) {
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) continue;
        size_t n = 0;
        void* data = SDL_LoadFile(path, &n);
        if (!data) continue;
        Buf b = {0};
        uint16_t pl = (uint16_t)strlen(names[i]);
        buf_put(&b, &pl, 2);
        buf_put(&b, names[i], pl);
        buf_put(&b, data, (uint32_t)n);
        send_msg(pe, M_FILE, b.buf, b.len);
        free(b.buf);
        SDL_free(data);
    }
    SDL_free(names);
}

static uint64_t ping_ms(Peer* pe) {
    uint64_t worst = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t t = SDL_GetTicksNS();
        if (!send_msg(pe, M_PING, &t, 8)) return 1000;
        int type;
        uint8_t* d;
        uint32_t len;
        if (wait_sock(pe->s, 0, 3000) != 1 || !recv_msg(pe->s, &type, &d, &len)) return 1000;
        free(d);
        uint64_t ms = (SDL_GetTicksNS() - t) / 1000000;
        if (ms > worst) worst = ms;
    }
    return worst;
}

static int host_start(const char* disc_root, int port, int friends) {
    sock_t ls = socket(AF_INET6, SOCK_STREAM, 0);
    int v6 = ls != BAD_SOCK, zero = 0, one = 1;
    if (!v6) ls = socket(AF_INET, SOCK_STREAM, 0);
    if (v6) setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&zero, sizeof zero);  /* IPv4 too */
#ifndef _WIN32
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#endif
    (void)one;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (v6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_any;
        a->sin6_port = htons((uint16_t)port);
        sl = sizeof *a;
    } else {
        struct sockaddr_in* a = (struct sockaddr_in*)&ss;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port = htons((uint16_t)port);
        sl = sizeof *a;
    }
    if (bind(ls, (struct sockaddr*)&ss, sl) != 0 || listen(ls, 8) != 0) {
        char m[200];
        snprintf(m, sizeof m, "Could not open port %d. Is another game already hosting on this PC?", port);
        fail(m);
        return 0;
    }
    uint32_t want_build = build_id(), want_disc = disc_id(disc_root);
    const char* myname = getenv("WPR_NET_NAME");
    snprintf(peers[0].name, sizeof peers[0].name, "%s", myname && *myname ? myname : "Host");
    while (npeers < 1 + friends) {
        SDL_LockMutex(lock);
        say("Online: waiting for friends to join (%d of %d) - port %d", npeers - 1, friends, port);
        SDL_UnlockMutex(lock);
        int r = wait_sock(ls, 0, 1 << 30);
        if (r < 0) return 0;
        if (r == 0) continue;
        sock_t s = accept(ls, NULL, NULL);
        if (s == BAD_SOCK) continue;
        sock_nodelay(s);
        Peer* pe = &peers[npeers];
        pe->s = s;
        pe->alive = 1;
        int type;
        uint8_t* d = NULL;
        uint32_t len;
        if (wait_sock(s, 0, 5000) != 1 || !recv_msg(s, &type, &d, &len) || type != M_HELLO || len < 17 + 32) {
            free(d);
            closesocket(s);
            pe->alive = 0;
            continue;
        }
        uint32_t magic, ver, build, disc;
        memcpy(&magic, d, 4);
        memcpy(&ver, d + 4, 4);
        memcpy(&build, d + 8, 4);
        memcpy(&disc, d + 12, 4);
        pe->nlocal = d[16];
        memcpy(pe->name, d + 17, 31);
        pe->name[31] = 0;
        free(d);
        const char* why = magic != NET_MAGIC || ver != NET_VERSION ? "Your game is a different version than the host's. Both need the same version."
                        : build != want_build ? "Your game build is different from the host's. Both need a build of the same game version."
                        : disc != want_disc ? "Your Wii Party disc is a different version or region than the host's."
                        : NULL;
        if (why) {
            send_msg(pe, M_REJECT, why, (uint32_t)strlen(why) + 1);
            closesocket(s);
            pe->alive = 0;
            fprintf(stderr, "[net] turned away %s: %s\n", pe->name, why);
            continue;
        }
        fprintf(stderr, "[net] %s joined with %d player(s)\n", pe->name, pe->nlocal);
        npeers++;
    }
    closesocket(ls);
    /* player slots: the host's players first, then each friend's in the order they joined */
    for (int p = 0; p < npeers; p++)
        for (int k = 0; k < (p ? peers[p].nlocal : nlocal_devs) && nplayers < MAX_PLAYERS; k++) owner[nplayers++] = p;
    uint64_t worst = 0;
    for (int p = 1; p < npeers; p++) { uint64_t ms = ping_ms(&peers[p]); if (ms > worst) worst = ms; }
    if (!getenv("WPR_NET_DELAY")) {  /* a friend's input has to cross the internet and come back in time */
        delay = (int)(worst * 60 / 1000) + 2;
        if (delay < 3) delay = 3;
        if (delay > 20) delay = 20;
    }
    char nand[1100];
    nand_root_of(disc_root, nand, sizeof nand);
    for (int p = 1; p < npeers; p++) {
        SDL_LockMutex(lock);
        say("Online: sending the save data to %s", peers[p].name);
        SDL_UnlockMutex(lock);
        send_nand(&peers[p], nand);
        Buf b = {0};
        uint8_t hdr[2 + MAX_PLAYERS] = {(uint8_t)p, (uint8_t)delay};
        for (int s = 0; s < MAX_PLAYERS; s++) hdr[2 + s] = (uint8_t)owner[s];
        buf_put(&b, hdr, sizeof hdr);
        for (size_t e = 0; e < sizeof shared_env / sizeof *shared_env; e++) {
            const char* v = getenv(shared_env[e]);
            char kv[96];
            int n = snprintf(kv, sizeof kv, "%s=%s", shared_env[e], v ? v : "");
            buf_put(&b, kv, (uint32_t)n + 1);
        }
        send_msg(&peers[p], M_START, b.buf, b.len);
        free(b.buf);
    }
    for (int p = 1; p < npeers; p++) SDL_DetachThread(SDL_CreateThread(host_recv, "net", (void*)(intptr_t)p));
    fprintf(stderr, "[net] hosting %d players, ping %llu ms, delay %d frames\n", nplayers, (unsigned long long)worst, delay);
    return 1;
}

static void clear_dir(const char* root) {
    int count = 0;
    char** names = SDL_GlobDirectory(root, NULL, 0, &count);
    for (int pass = 0; pass < 8; pass++)  /* files first, then folders once they are empty */
        for (int i = count - 1; names && i >= 0; i--) {
            char path[1400];
            snprintf(path, sizeof path, "%s/%s", root, names[i]);
            SDL_RemovePath(path);
        }
    SDL_free(names);
}

static int join_start(const char* disc_root, const char* where) {
    char host[256];
    snprintf(host, sizeof host, "%s", where);
    int port = DEFAULT_PORT;
    char* colon = strrchr(host, ':');
    if (host[0] == '[') {  /* [IPv6]:port */
        char* e = strchr(host, ']');
        if (e && e[1] == ':') port = atoi(e + 2);
        if (e) *e = 0;
        memmove(host, host + 1, strlen(host));
    } else if (colon && strchr(host, ':') == colon) {  /* name:port (a bare IPv6 address has several) */
        port = atoi(colon + 1);
        *colon = 0;
    }
    SDL_LockMutex(lock);
    say("Online: connecting to %s...", where);
    SDL_UnlockMutex(lock);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { fail("Couldn't find that address. Check what you typed."); return 0; }
    sock_t s = BAD_SOCK;
    for (struct addrinfo* a = res; a && s == BAD_SOCK; a = a->ai_next) {
        s = socket(a->ai_family, SOCK_STREAM, 0);
        if (s == BAD_SOCK) continue;
        sock_blocking(s, 0);
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        int ok = wait_sock(s, 1, 10000), err = 0;
        if (ok < 0) { closesocket(s); freeaddrinfo(res); return 0; }
        socklen_t el = sizeof err;
        if (ok) getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (!ok || err) { closesocket(s); s = BAD_SOCK; continue; }
        sock_blocking(s, 1);
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) {
        fail("Couldn't reach the host. Check the address, and that the host has pressed Host. "
             "If it still doesn't work, see the Online tab in the launcher.");
        return 0;
    }
    sock_nodelay(s);
    Peer* pe = &peers[0];
    pe->s = s;
    pe->alive = 1;
    snprintf(pe->name, sizeof pe->name, "the host");
    uint8_t hello[17 + 32] = {0};
    uint32_t v[4] = {NET_MAGIC, NET_VERSION, build_id(), disc_id(disc_root)};
    memcpy(hello, v, 16);
    hello[16] = (uint8_t)nlocal_devs;
    const char* myname = getenv("WPR_NET_NAME");
    snprintf((char*)hello + 17, 32, "%s", myname && *myname ? myname : "Friend");
    send_msg(pe, M_HELLO, hello, sizeof hello);
    SDL_LockMutex(lock);
    say("Online: connected, waiting for the host to start...");
    SDL_UnlockMutex(lock);

    char nand[1100];
    nand_root_of(disc_root, nand, sizeof nand);
    strncat(nand, "-online", sizeof nand - strlen(nand) - 1);  /* the host's saves, kept apart from ours */
    SDL_CreateDirectory(nand);
    clear_dir(nand);
    for (;;) {
        int r = wait_sock(s, 0, 1 << 30);
        if (r < 0) return 0;
        if (r == 0) continue;
        int type;
        uint8_t* d;
        uint32_t len;
        if (!recv_msg(s, &type, &d, &len)) { fail("The host closed the connection."); return 0; }
        if (type == M_REJECT) { fail((const char*)d); free(d); return 0; }
        if (type == M_PING) send_msg(pe, M_PONG, d, len);
        if (type == M_FILE && len >= 2) {
            uint16_t pl;
            memcpy(&pl, d, 2);
            if (2u + pl <= len && pl < 1000) {
                char rel[1001], path[2200];
                memcpy(rel, d + 2, pl);
                rel[pl] = 0;
                if (strlen(rel) == pl && !strstr(rel, "..") && rel[0] != '/' && rel[0] != '\\' && !strchr(rel, ':')) {
                    snprintf(path, sizeof path, "%s/%s", nand, rel);
                    char* slash = strrchr(path, '/');
                    if (slash) { *slash = 0; SDL_CreateDirectory(path); *slash = '/'; }
                    SDL_SaveFile(path, d + 2 + pl, len - 2 - pl);
                }
            }
        }
        if (type == M_START && len >= 2 + MAX_PLAYERS) {
            me = d[0];
            delay = d[1];
            for (int sl = 0; sl < MAX_PLAYERS; sl++) owner[sl] = d[2 + sl] == 255 ? -1 : d[2 + sl];
            for (uint32_t o = 2 + MAX_PLAYERS; o < len;) {
                char* kv = (char*)d + o;
                size_t n = strlen(kv);
                char* eq = strchr(kv, '=');
                if (eq) { *eq = 0; set_env(kv, eq + 1); }
                o += (uint32_t)n + 1;
            }
            free(d);
            break;
        }
        free(d);
    }
    set_env("WPR_NAND", nand);
    int mine = 0;
    for (int sl = 0; sl < MAX_PLAYERS; sl++) {
        if (owner[sl] == me) mine++;
        if (owner[sl] >= 0) nplayers++;
    }
    if (mine < nlocal_devs) nlocal_devs = mine;  /* the host had no room for all of ours */
    SDL_DetachThread(SDL_CreateThread(client_recv, "net", NULL));
    fprintf(stderr, "[net] joined as peer %d, delay %d frames\n", me, delay);
    return 1;
}

int net_start(const char* disc_root) {
    const char* host = getenv("WPR_NET_HOST");
    const char* join = getenv("WPR_NET_JOIN");
    if (!(host && *host) && !(join && *join)) return 1;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    lock = SDL_CreateMutex();
    cond = SDL_CreateCondition();
    for (int p = 0; p < MAX_PEERS; p++) peers[p].send_lock = SDL_CreateMutex();
    const char* d = getenv("WPR_NET_DELAY");
    if (d && atoi(d) > 0) delay = atoi(d);
    /* the devices that play on this PC: 0 = keyboard and mouse, 1.. = gamepads */
    const char* s = getenv("WPR_NET_LOCAL");
    for (s = s && *s ? s : "0"; *s && nlocal_devs < MAX_PLAYERS;) {
        int dev = atoi(s);
        if (dev >= 0 && dev < MAX_PLAYERS) local_devs[nlocal_devs++] = dev;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    if (!nlocal_devs) local_devs[nlocal_devs++] = 0;
    int ok;
    if (host && *host) {
        const char* f = getenv("WPR_NET_PLAYERS");  /* how many friends to wait for */
        int friends = f ? atoi(f) : 1;
        if (friends < 1) friends = 1;
        if (friends > MAX_PEERS - 1) friends = MAX_PEERS - 1;
        mode = 1;
        ok = host_start(disc_root, atoi(host) > 0 ? atoi(host) : DEFAULT_PORT, friends);
    } else {
        mode = 2;
        ok = join_start(disc_root, join);
    }
    if (!ok) { mode = 0; return 0; }
    /* the first frames, before anyone's input can arrive, are the same neutral input everywhere */
    for (int f = 0; f < delay; f++) {
        for (int sl = 0; sl < MAX_PLAYERS; sl++) frames[f][sl] = neutral(sl);
        frame_tag[f] = (uint32_t)f + 1;
    }
    memcpy(cur, frames[0], sizeof cur);
    SDL_LockMutex(lock);
    say("Online: %d players", nplayers);
    SDL_UnlockMutex(lock);
    return 1;
}
