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
 * Messages are ENet packets (reliable, in order, over UDP): [u8 type][payload], little endian.
 *
 * Finding each other: the host makes up a room code (the launcher does) and friends type it in.
 * Both sides ask a public STUN server which address their router gives them, and swap their
 * addresses through a public message board (ntfy.sh, topic named after the code). Then the friend
 * connects to every address of the host while the host sends a few packets the other way, which
 * opens both routers ("hole punching"). Same PC and same home network work through the local
 * addresses. Joining by IP address (with a forwarded UDP port, or a VPN) still works too.
 */
#include <enet/enet.h>
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netplay.h"
#include "ppc_context.h"

#define NET_MAGIC 0x504E5057u  /* "WPNP" */
#define NET_VERSION 2
#define DEFAULT_PORT 47123
#define MAX_PEERS 4
#define MAX_INCOMING 8
#define RING 512
#define REMOTE_BYTES 24
#define MAX_EPS 6

enum { M_HELLO = 1, M_REJECT, M_PING, M_PONG, M_FILE, M_START, M_INPUT, M_FRAME, M_HASH, M_DESYNC, M_BYE, M_DIR };

typedef struct Msg { struct Msg* next; int type; uint32_t len; uint8_t* data; } Msg;

typedef struct {
    ENetPeer* ep;
    Msg *head, *tail;         /* received while setting up, not handled yet */
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
static Peer incoming[MAX_INCOMING];  /* host: connections that haven't said hello yet */
static int npeers = 1, nplayers;
static int owner[MAX_PLAYERS] = {-1, -1, -1, -1};  /* which peer plays each player slot */
static int nlocal_devs, local_devs[MAX_PLAYERS];     /* our devices, in slot order */

/* Lock order: elock (ENet, only the network thread services it) -> lock (game state) -> olock. */
static ENetHost* enh;
static SDL_Mutex* elock;
static int accepting;          /* host: still letting friends in */
static int dispatching;        /* setup done: the network thread handles messages itself */

typedef struct Out { struct Out* next; ENetPeer* ep; ENetPacket* pk; } Out;
static SDL_Mutex* olock;       /* the outbox: sending never waits for elock */
static Out *out_head, *out_tail;

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

static void say(const char* fmt, ...) {
    va_list ap;
    SDL_LockMutex(lock);
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[net] %s\n", status);
    SDL_UnlockMutex(lock);
}

const char* net_status(void) {
    static char copy[160];
    if (!lock) return NULL;
    SDL_LockMutex(lock);
    memcpy(copy, status, sizeof copy);
    SDL_UnlockMutex(lock);
    return copy[0] ? copy : NULL;
}

static void addr_str(const ENetAddress* a, char* out, size_t n) {
    char ip[64] = "?";
    enet_address_get_host_ip(a, ip, sizeof ip);
    snprintf(out, n, "%s:%u", ip, (unsigned)a->port);
}

/* "1.2.3.4:5678" or "name[:port]" */
static int parse_addr(const char* s, ENetAddress* a, int default_port) {
    char host[256];
    snprintf(host, sizeof host, "%s", s);
    a->port = (enet_uint16)default_port;
    char* colon = strrchr(host, ':');
    if (colon) { a->port = (enet_uint16)atoi(colon + 1); *colon = 0; }
    return host[0] && enet_address_set_host(a, host) == 0;
}

/* ------------------------------------------------------------------ messages */
static int send_msg(Peer* p, int type, const void* data, uint32_t len) {
    ENetPeer* ep = p->ep;  /* read once: the network thread clears it when the peer drops */
    if (!p->alive || !ep) return 0;
    ENetPacket* pk = enet_packet_create(NULL, len + 1, ENET_PACKET_FLAG_RELIABLE);
    if (!pk) return 0;
    pk->data[0] = (uint8_t)type;
    if (len) memcpy(pk->data + 1, data, len);
    Out* o = malloc(sizeof *o);
    o->next = NULL;
    o->ep = ep;
    o->pk = pk;
    SDL_LockMutex(olock);
    if (out_tail) out_tail->next = o; else out_head = o;
    out_tail = o;
    SDL_UnlockMutex(olock);
    return 1;
}

/* elock held */
static void send_outbox(void) {
    SDL_LockMutex(olock);
    Out* o = out_head;
    out_head = out_tail = NULL;
    SDL_UnlockMutex(olock);
    while (o) {
        Out* next = o->next;
        if (!o->ep || enet_peer_send(o->ep, 0, o->pk) < 0) enet_packet_destroy(o->pk);
        free(o);
        o = next;
    }
}

/* A whole message from p, received while setting up. Waits up to ms, keeping the window alive.
 * Returns 1 (*data is malloc'd and zero-terminated: free it), 0 timeout, -1 p is gone, -2 the window
 * was closed. Main thread only. */
static int recv_msg(Peer* p, int ms, int* type, uint8_t** data, uint32_t* len) {
    uint64_t end = SDL_GetTicks() + (uint64_t)ms;
    for (;;) {
        SDL_LockMutex(elock);
        Msg* m = p->head;
        if (m) { p->head = m->next; if (!p->head) p->tail = NULL; }
        int gone = !p->alive;
        SDL_UnlockMutex(elock);
        if (m) {
            *type = m->type;
            *data = m->data;
            *len = m->len;
            free(m);
            return 1;
        }
        if (gone) return -1;
        if (!platform_pump()) return -2;
        platform_set_fps(-1);
        if (SDL_GetTicks() >= end) return 0;
        SDL_DelayNS(2000000);
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
        snprintf(status, sizeof status, "Online: OUT OF SYNC with %s at frame %u - please restart", peers[p].name, f);
        fprintf(stderr, "[net] %s\n", status);
        for (int q = 1; q < npeers; q++) if (peers[q].alive) send_msg(&peers[q], M_DESYNC, &f, 4);
        return;
    }
}

/* network thread (elock held), once playing */
static void host_on_msg(int p, int type, const uint8_t* d, uint32_t len) {
    Peer* pe = &peers[p];
    if (type == M_PING) { send_msg(pe, M_PONG, d, len); return; }
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
}

static void host_peer_left(int p) {
    SDL_LockMutex(lock);
    peers[p].alive = 0;
    snprintf(status, sizeof status, "Online: %s left the game", peers[p].name);
    fprintf(stderr, "[net] %s\n", status);
    for (uint32_t f = host_cur; f <= host_newest; f++) try_complete(f);
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(lock);
}

static void client_on_msg(int type, const uint8_t* d, uint32_t len) {
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
        snprintf(status, sizeof status, "Online: OUT OF SYNC at frame %u - please restart", f);
        fprintf(stderr, "[net] %s\n", status);
        SDL_UnlockMutex(lock);
    } else if (type == M_PING) {
        send_msg(&peers[0], M_PONG, d, len);
    }
}

static void client_host_left(void) {
    SDL_LockMutex(lock);
    peers[0].alive = 0;
    host_gone = 1;
    snprintf(status, sizeof status, "Online: the host left the game - close this window");
    fprintf(stderr, "[net] %s\n", status);
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(lock);
}

/* elock held: hand messages that came in while setting up to the playing handlers */
static void start_dispatching(void) {
    dispatching = 1;
    for (int p = 0; p < npeers; p++) {
        Msg* m = peers[p].head;
        peers[p].head = peers[p].tail = NULL;
        while (m) {
            Msg* next = m->next;
            if (mode == 1) host_on_msg(p, m->type, m->data, m->len); else client_on_msg(m->type, m->data, m->len);
            free(m->data);
            free(m);
            m = next;
        }
    }
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
            if (mode == 2) snprintf(status, sizeof status, "Online: waiting for the other players...");
            else {
                const char* who = "a friend";
                for (int s = 0; s < MAX_PLAYERS; s++)
                    if (owner[s] > 0 && pend_tag[f % RING][s] != f + 1) who = peers[owner[s]].name;
                snprintf(status, sizeof status, "Online: waiting for %s...", who);
            }
        }
    }
    if (waited && !desynced && !host_gone) snprintf(status, sizeof status, "Online: %d players", nplayers);
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

/* ------------------------------------------------------------------ the network thread */
static uint32_t stun_tx[3];                /* the STUN request we're waiting for an answer to */
static ENetAddress stun_result;            /* our address as the internet sees it (host 0: unknown) */
typedef struct { ENetAddress a; uint64_t until; } Punch;
static Punch punches[16];
static int npunches;

/* elock held, inside enet_host_service: STUN answers and punch packets aren't for ENet */
static int ENET_CALLBACK intercept(ENetHost* h, ENetEvent* ev) {
    (void)ev;
    const uint8_t* d = h->receivedData;
    size_t n = h->receivedDataLength;
    if (n >= 8 && !memcmp(d, "WPRPUNCH", 8)) return 1;
    if (n < 20 || d[0] != 0x01 || d[1] != 0x01 || d[4] != 0x21 || d[5] != 0x12 || d[6] != 0xA4 || d[7] != 0x42) return 0;
    if (memcmp(d + 8, stun_tx, 12)) return 1;
    size_t end = 20 + (size_t)(d[2] << 8 | d[3]);
    if (end > n) end = n;
    for (size_t o = 20; o + 4 <= end;) {  /* XOR-MAPPED-ADDRESS (or the old MAPPED-ADDRESS), IPv4 */
        int at = d[o] << 8 | d[o + 1], al = d[o + 2] << 8 | d[o + 3];
        if ((at == 0x0020 || at == 0x0001) && al >= 8 && o + 12 <= end && d[o + 5] == 0x01) {
            uint16_t port = (uint16_t)(d[o + 6] << 8 | d[o + 7]);
            uint8_t ip[4];
            memcpy(ip, d + o + 8, 4);
            if (at == 0x0020) {
                port ^= 0x2112;
                for (int k = 0; k < 4; k++) ip[k] ^= d[4 + k];
            }
            memcpy(&stun_result.host, ip, 4);  /* network order already */
            stun_result.port = port;
            return 1;
        }
        o += 4 + (size_t)((al + 3) & ~3);
    }
    return 1;
}

/* elock held */
static void punch_tick(void) {
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (!npunches || now - last < 150) return;
    last = now;
    ENetBuffer b = {.data = "WPRPUNCH", .dataLength = 8};
    for (int i = 0; i < npunches; i++)
        if (now < punches[i].until) enet_socket_send(enh->socket, &punches[i].a, &b, 1);
}

static void add_punch(const ENetAddress* a) {
    SDL_LockMutex(elock);
    uint64_t now = SDL_GetTicks();
    int slot = -1;
    /* the same address again (a friend retrying) or a slot whose 15 seconds are over */
    for (int i = 0; i < npunches && slot < 0; i++)
        if (punches[i].a.host == a->host && punches[i].a.port == a->port) slot = i;
    for (int i = 0; i < npunches && slot < 0; i++)
        if (now >= punches[i].until) slot = i;
    if (slot < 0 && npunches < (int)(sizeof punches / sizeof *punches)) slot = npunches++;
    if (slot >= 0) {
        punches[slot].a = *a;
        punches[slot].until = now + 15000;
    }
    SDL_UnlockMutex(elock);
}

static Peer* peer_of(ENetPeer* ep) { return (Peer*)ep->data; }

/* elock held */
static void on_event(ENetEvent* ev) {
    Peer* pe = ev->peer ? peer_of(ev->peer) : NULL;
    if (ev->type == ENET_EVENT_TYPE_CONNECT) {
        if (mode != 1 || pe) return;  /* a joiner looks at its connection attempts itself */
        if (!accepting) { enet_peer_disconnect_now(ev->peer, 0); return; }
        for (int i = 0; i < MAX_INCOMING; i++)
            if (!incoming[i].ep) {
                memset(&incoming[i], 0, sizeof incoming[i]);
                incoming[i].ep = ev->peer;
                incoming[i].alive = 1;
                ev->peer->data = &incoming[i];
                return;
            }
        enet_peer_disconnect_now(ev->peer, 0);
    } else if (ev->type == ENET_EVENT_TYPE_RECEIVE) {
        ENetPacket* pk = ev->packet;
        if (pe && pk->dataLength >= 1) {
            if (dispatching && pe >= peers && pe < peers + MAX_PEERS) {
                if (mode == 1) host_on_msg((int)(pe - peers), pk->data[0], pk->data + 1, (uint32_t)pk->dataLength - 1);
                else client_on_msg(pk->data[0], pk->data + 1, (uint32_t)pk->dataLength - 1);
            } else if (!dispatching) {
                Msg* m = malloc(sizeof *m);
                m->next = NULL;
                m->type = pk->data[0];
                m->len = (uint32_t)pk->dataLength - 1;
                m->data = malloc(m->len + 1);
                memcpy(m->data, pk->data + 1, m->len);
                m->data[m->len] = 0;
                if (pe->tail) pe->tail->next = m; else pe->head = m;
                pe->tail = m;
            }
        }
        enet_packet_destroy(pk);
    } else if (ev->type == ENET_EVENT_TYPE_DISCONNECT) {
        if (!pe) return;
        ev->peer->data = NULL;
        pe->ep = NULL;
        if (dispatching && pe >= peers && pe < peers + MAX_PEERS) {
            if (mode == 1) host_peer_left((int)(pe - peers)); else client_host_left();
        } else {
            pe->alive = 0;
        }
    }
}

static int SDLCALL net_thread(void* arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(elock);
        send_outbox();
        ENetEvent ev;
        while (enet_host_service(enh, &ev, 0) > 0) on_event(&ev);
        send_outbox();
        punch_tick();
        enet_host_flush(enh);
        SDL_UnlockMutex(elock);
        SDL_DelayNS(1000000);
    }
    return 0;
}

/* ------------------------------------------------------------------ finding each other */
/* Our own addresses: what the internet sees (STUN), this PC on the home network, and this PC. */
static void stun_ask(const char* server) {
    ENetAddress a;
    if (!parse_addr(server, &a, 3478)) return;
    uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    SDL_LockMutex(elock);
    for (int k = 0; k < 3; k++) stun_tx[k] = (uint32_t)rand() ^ (uint32_t)SDL_GetTicksNS() ^ ((uint32_t)k << 24);
    memcpy(req + 8, stun_tx, 12);
    ENetBuffer b = {.data = req, .dataLength = sizeof req};
    enet_socket_send(enh->socket, &a, &b, 1);
    SDL_UnlockMutex(elock);
}

static int my_endpoints(char* out, size_t n) {
    const char* env = getenv("WPR_NET_STUN");
    const char* servers[] = {env && *env ? env : "stun.l.google.com:19302", "stun.cloudflare.com:3478", "stun1.l.google.com:19302"};
    ENetAddress pub = {0};
    for (int tries = 0; tries < 3 && !pub.host; tries++) {
        stun_ask(servers[tries]);
        for (int w = 0; w < 30 && !pub.host; w++) {  /* up to 1.5 s */
            SDL_DelayNS(50000000);
            SDL_LockMutex(elock);
            pub = stun_result;
            SDL_UnlockMutex(elock);
        }
    }
    out[0] = 0;
    size_t used = 0;
    char ep[64];
    if (pub.host) { addr_str(&pub, ep, sizeof ep); used += (size_t)snprintf(out + used, n - used, "%s", ep); }
    /* the home network address: the one a packet to the internet would leave from (nothing is sent) */
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress probe, lan = {0};
    if (s != ENET_SOCKET_NULL) {
        enet_address_set_host_ip(&probe, "8.8.8.8");
        probe.port = 53;
        if (enet_socket_connect(s, &probe) == 0) enet_socket_get_address(s, &lan);
        enet_socket_destroy(s);
    }
    if (lan.host && lan.host != pub.host) {
        lan.port = enh->address.port;
        addr_str(&lan, ep, sizeof ep);
        used += (size_t)snprintf(out + used, n - used, "%s%s", used ? "," : "", ep);
    }
    snprintf(out + used, n - used, "%s127.0.0.1:%u", used ? "," : "", (unsigned)enh->address.port);
    fprintf(stderr, "[net] our addresses: %s%s\n", out, pub.host ? "" : " (no answer from the STUN servers)");
    return pub.host != 0;
}

static int parse_endpoints(const char* list, ENetAddress* out, int max) {
    int n = 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char* t = strtok(buf, ","); t && n < max; t = strtok(NULL, ","))
        if (strchr(t, ':') && parse_addr(t, &out[n], DEFAULT_PORT)) n++;
    return n;
}

/* A tiny HTTP/1.0 client for the message board, over ENet's portable sockets. Calls line() for
 * every line of the body (until it returns nonzero) and gives up after ms of silence or when
 * *stop is set. Returns 1 if the server answered 200. */
static const char* board(void) {
    const char* s = getenv("WPR_NET_SIGNAL");
    return s && *s ? s : "ntfy.sh";
}

/* Waits for a non-blocking connect. Windows reports a refused connection only in the exception set,
 * which ENet's wait doesn't ask for (it would sit out the whole timeout). */
static int wait_connected(ENetSocket s, int ms) {
#ifdef _WIN32
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(0, NULL, &w, &e, &tv) > 0 && !FD_ISSET(s, &e) && FD_ISSET(s, &w);
#else
    enet_uint32 w = ENET_SOCKET_WAIT_SEND;
    int err = 0;
    return enet_socket_wait(s, &w, ms) == 0 && (w & ENET_SOCKET_WAIT_SEND) &&
           enet_socket_get_option(s, ENET_SOCKOPT_ERROR, &err) == 0 && !err;
#endif
}

static int http(const char* method, const char* path, const char* body, int (*line)(const char*, void*), void* ctx,
                int ms, volatile int* stop) {
    ENetAddress a;
    char host[128];
    snprintf(host, sizeof host, "%s", board());
    char* colon = strchr(host, ':');
    if (colon) *colon = 0;
    if (!parse_addr(board(), &a, 80)) return 0;
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
    if (s == ENET_SOCKET_NULL) return 0;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    int ok = 0;
    if (enet_socket_connect(s, &a) < 0 || !wait_connected(s, 8000)) {
        enet_socket_destroy(s);
        return 0;
    }
    enet_uint32 w;
    char req[1024];
    int rl = snprintf(req, sizeof req,
                      "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: wiiparty-native\r\nContent-Length: %u\r\n\r\n%s", method,
                      path, host, body ? (unsigned)strlen(body) : 0u, body ? body : "");
    for (int sent = 0; sent < rl;) {
        ENetBuffer b = {.data = req + sent, .dataLength = (size_t)(rl - sent)};
        int k = enet_socket_send(s, NULL, &b, 1);
        if (k < 0) { enet_socket_destroy(s); return 0; }
        if (k == 0) { w = ENET_SOCKET_WAIT_SEND; enet_socket_wait(s, &w, 1000); }
        sent += k;
    }
    char buf[8192];
    size_t have = 0;
    int in_body = 0, status_ok = 0, done = 0;
    uint64_t quiet_until = SDL_GetTicks() + (uint64_t)ms;
    while (!done && !(stop && *stop) && SDL_GetTicks() < quiet_until) {
        w = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(s, &w, 200) < 0) break;
        if (!(w & ENET_SOCKET_WAIT_RECEIVE)) continue;
        if (have >= sizeof buf - 1) have = 0;  /* a line this long isn't ours */
        ENetBuffer b = {.data = buf + have, .dataLength = sizeof buf - 1 - have};
        int k = enet_socket_receive(s, NULL, &b, 1);
        if (k <= 0) break;  /* readable and nothing to read: the server closed */
        have += (size_t)k;
        quiet_until = SDL_GetTicks() + (uint64_t)ms;
        buf[have] = 0;
        char* nl;
        while (!done && (nl = strchr(buf, '\n'))) {
            *nl = 0;
            if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
            if (!in_body) {
                if (!strncmp(buf, "HTTP/", 5)) status_ok = strstr(buf, " 200") != NULL;
                else if (!buf[0]) in_body = 1;
            } else if (line && buf[0]) {
                done = line(buf, ctx);
            }
            size_t used = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, have - used + 1);
            have -= used;
        }
    }
    if (!done && in_body && have && line) line(buf, ctx);
    ok = status_ok;
    enet_socket_destroy(s);
    return ok;
}

/* The string value of "key" in a one-line JSON object (no escapes: our messages don't need them). */
static int json_str(const char* json, const char* key, char* out, size_t n) {
    char k[32];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char* p = json;
    for (;;) {  /* a key is followed by a colon ("event":"message" has "message" as a value too) */
        p = strstr(p, k);
        if (!p) return 0;
        p += strlen(k);
        while (*p == ' ') p++;
        if (*p == ':') break;
    }
    p++;
    while (*p == ' ') p++;
    if (*p++ != '"') return 0;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/* the text of a message-board event line, if it is a message: {"id":..,"event":"message",..,"message":"..."} */
static int board_message(const char* json, char* out, size_t n) {
    char ev[16];
    return json_str(json, "event", ev, sizeof ev) && !strcmp(ev, "message") && json_str(json, "message", out, n);
}

static char room[16];        /* the room code, upper case */
static char topic[40];

static void room_topic(const char* code) {
    uint64_t h = 1469598103934665603ull;
    const char* salt = "wii-party-native room ";
    for (const char* p = salt; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (const char* p = code; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    snprintf(topic, sizeof topic, "wpr%016llx", (unsigned long long)h);
}

/* A room code is 6 letters and digits with at least one of each (dashes and spaces are ignored);
 * anything else is an address or a computer's name. */
static int as_room_code(const char* s, char* out, size_t n) {
    char code[8];
    size_t k = 0;
    out[0] = 0;
    for (; *s; s++) {
        if (*s == '-' || *s == ' ') continue;
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) || k >= 6) return 0;
        code[k++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    }
    code[k] = 0;
    if (k != 6 || strcspn(code, "0123456789") == k || strspn(code, "0123456789") == k || n <= k) return 0;
    memcpy(out, code, k + 1);
    return 1;
}

/* host: tells friends where we are, then punches toward every friend who answers */
static volatile int host_board_stop;
static char host_post_id[64];
static int host_friends;

static int host_board_line(const char* json, void* ctx) {
    (void)ctx;
    char msg[600];
    if (!board_message(json, msg, sizeof msg) || strncmp(msg, "WPR2 J ", 7)) return 0;
    ENetAddress eps[MAX_EPS];
    int n = parse_endpoints(msg + 7, eps, MAX_EPS);
    fprintf(stderr, "[net] a friend is coming: %s\n", msg + 7);
    for (int i = 0; i < n; i++) add_punch(&eps[i]);
    return 0;
}

static int publish_line(const char* json, void* ctx) {
    json_str(json, "id", ctx, sizeof host_post_id);
    return 1;
}

static int SDLCALL host_board_thread(void* arg) {
    (void)arg;
    char eps[400], body[480], path[160];
    my_endpoints(eps, sizeof eps);
    snprintf(body, sizeof body, "WPR2 H %s", eps);
    snprintf(path, sizeof path, "/%s", topic);
    int posted = 0;
    for (int tries = 0; !host_board_stop && !posted; tries++) {
        posted = http("POST", path, body, publish_line, host_post_id, 8000, &host_board_stop);
        if (!posted) {
            say("Online: can't reach the room service (%s) - check the internet; friends can still join by IP address", board());
            SDL_DelayNS(3000000000ull);
        }
    }
    /* not when a friend already joined (by IP) while the post was on its way: the game shows that */
    if (posted && !host_board_stop && npeers == 1) say("Online: room %s - waiting for friends to join (0 of %d)", room, host_friends);
    /* listen for friends; the board keeps the connection open and sends a keepalive now and then */
    snprintf(path, sizeof path, "/%s/json?since=%s", topic, host_post_id[0] ? host_post_id : "30s");
    while (!host_board_stop) {
        if (!http("GET", path, NULL, host_board_line, NULL, 70000, &host_board_stop) && !host_board_stop)
            SDL_DelayNS(2000000000ull);
        /* after a reconnect, only what's new */
        snprintf(path, sizeof path, "/%s/json?since=10s", topic);
    }
    return 0;
}

/* joiner: finds the host's addresses and tells the host ours */
typedef struct { char eps[400]; int found; } HostLookup;

static int join_board_line(const char* json, void* ctx) {
    HostLookup* l = ctx;
    char msg[600];
    if (board_message(json, msg, sizeof msg) && !strncmp(msg, "WPR2 H ", 7)) {
        snprintf(l->eps, sizeof l->eps, "%s", msg + 7);  /* the newest one wins */
        l->found = 1;
    }
    return 0;
}

typedef struct { HostLookup host; char mine[400]; int board_ok, done; } JoinBoard;

static int SDLCALL join_board_thread(void* arg) {
    JoinBoard* j = arg;
    char path[160], body[480];
    my_endpoints(j->mine, sizeof j->mine);
    snprintf(path, sizeof path, "/%s/json?poll=1&since=3h", topic);
    for (int tries = 0; tries < 20 && !j->host.found; tries++) {  /* the host may still be starting */
        if (http("GET", path, NULL, join_board_line, &j->host, 8000, NULL)) j->board_ok = 1;
        if (!j->host.found) SDL_DelayNS(3000000000ull);
    }
    if (j->host.found) {
        snprintf(path, sizeof path, "/%s", topic);
        snprintf(body, sizeof body, "WPR2 J %s", j->mine);
        http("POST", path, body, NULL, NULL, 8000, NULL);
    }
    j->done = 1;
    return 0;
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
    say("Online: %s", msg);
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
        if (!SDL_GetPathInfo(path, &info)) continue;
        if (info.type == SDL_PATHTYPE_DIRECTORY) {  /* empty ones matter too: the game saves through /tmp */
            send_msg(pe, M_DIR, names[i], (uint32_t)strlen(names[i]));
            continue;
        }
        if (info.type != SDL_PATHTYPE_FILE) continue;
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
        do {
            if (recv_msg(pe, 3000, &type, &d, &len) != 1) return 1000;
            free(d);
        } while (type != M_PONG);
        uint64_t ms = (SDL_GetTicksNS() - t) / 1000000;
        if (ms > worst) worst = ms;
    }
    return worst;
}

static int open_host(int port) {
    ENetAddress a = {.host = ENET_HOST_ANY, .port = (enet_uint16)port};
    enh = enet_host_create(&a, 16, 1, 0, 0);
    if (!enh) return 0;
    enh->intercept = intercept;
    SDL_DetachThread(SDL_CreateThread(net_thread, "net", NULL));
    return 1;
}

static int host_start(const char* disc_root, int port, int friends) {
    if (!open_host(port)) {
        char m[200];
        snprintf(m, sizeof m, "Could not open port %d. Is another game already hosting on this PC?", port);
        fail(m);
        return 0;
    }
    accepting = 1;
    npeers = 1;
    uint32_t want_build = build_id(), want_disc = disc_id(disc_root);
    const char* myname = getenv("WPR_NET_NAME");
    snprintf(peers[0].name, sizeof peers[0].name, "%s", myname && *myname ? myname : "Host");
    if (room[0]) {
        say("Online: opening room %s...", room);
        host_board_stop = 0;
        host_friends = friends;
        SDL_DetachThread(SDL_CreateThread(host_board_thread, "room", NULL));
    }
    int shown = -1;
    while (npeers < 1 + friends) {
        if (shown != npeers) {
            shown = npeers;
            if (room[0]) say("Online: room %s - waiting for friends to join (%d of %d)", room, npeers - 1, friends);
            else say("Online: waiting for friends to join (%d of %d) - port %d", npeers - 1, friends, port);
        }
        if (!platform_pump()) { host_board_stop = 1; return 0; }
        platform_set_fps(-1);
        SDL_DelayNS(5000000);
        SDL_LockMutex(elock);
        for (int i = 0; i < MAX_INCOMING && npeers < 1 + friends; i++) {
            Peer* in = &incoming[i];
            if (!in->ep) continue;
            Msg* m = in->head;
            if (!m) continue;
            in->head = m->next;
            if (!in->head) in->tail = NULL;
            const uint8_t* d = m->data;
            const char* why = NULL;
            if (m->type != M_HELLO || m->len < 17 + 32) {
                why = "That isn't a Wii Party game.";
            } else {
                uint32_t magic, ver, build, disc;
                memcpy(&magic, d, 4);
                memcpy(&ver, d + 4, 4);
                memcpy(&build, d + 8, 4);
                memcpy(&disc, d + 12, 4);
                in->nlocal = d[16];
                memcpy(in->name, d + 17, 31);
                in->name[31] = 0;
                why = magic != NET_MAGIC || ver != NET_VERSION ? "Your game is a different version than the host's. Both need the same version."
                    : build != want_build ? "Your game build is different from the host's. Both need a build of the same game version."
                    : disc != want_disc ? "Your Wii Party disc is a different version or region than the host's."
                    : NULL;
            }
            free(m->data);
            free(m);
            if (why) {
                ENetPacket* pk = enet_packet_create(NULL, strlen(why) + 2, ENET_PACKET_FLAG_RELIABLE);
                pk->data[0] = M_REJECT;
                memcpy(pk->data + 1, why, strlen(why) + 1);
                enet_peer_send(in->ep, 0, pk);
                enet_peer_disconnect_later(in->ep, 0);
                fprintf(stderr, "[net] turned away %s: %s\n", in->name, why);
                in->ep->data = NULL;
                memset(in, 0, sizeof *in);
                continue;
            }
            Peer* pe = &peers[npeers++];
            memcpy(pe, in, sizeof *pe);
            pe->ep->data = pe;
            memset(in, 0, sizeof *in);
            fprintf(stderr, "[net] %s joined with %d player(s)\n", pe->name, pe->nlocal);
        }
        /* a friend who left before we started frees their place */
        for (int p = 1; p < npeers; p++)
            if (!peers[p].alive) {
                fprintf(stderr, "[net] %s left before the start\n", peers[p].name);
                memmove(&peers[p], &peers[p + 1], sizeof(Peer) * (size_t)(npeers - p - 1));
                npeers--;
                for (int q = p; q < npeers; q++) if (peers[q].ep) peers[q].ep->data = &peers[q];
                p--;
            }
        SDL_UnlockMutex(elock);
    }
    SDL_LockMutex(elock);
    accepting = 0;
    for (int i = 0; i < MAX_INCOMING; i++)  /* spare connections (a friend tried several of our addresses) */
        if (incoming[i].ep) { enet_peer_disconnect_now(incoming[i].ep, 0); incoming[i].ep->data = NULL; }
    SDL_UnlockMutex(elock);
    host_board_stop = 1;
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
        say("Online: sending the save data to %s", peers[p].name);
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
    SDL_LockMutex(elock);
    start_dispatching();
    SDL_UnlockMutex(elock);
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

/* Tries every address at once and keeps the first that answers. */
static ENetPeer* connect_any(const ENetAddress* eps, int n, int ms) {
    ENetPeer* tries[MAX_EPS] = {0};
    SDL_LockMutex(elock);
    for (int i = 0; i < n && i < MAX_EPS; i++) {
        tries[i] = enet_host_connect(enh, &eps[i], 1, 0);
        if (tries[i]) tries[i]->data = NULL;
    }
    SDL_UnlockMutex(elock);
    ENetPeer* won = NULL;
    for (uint64_t end = SDL_GetTicks() + (uint64_t)ms; !won && SDL_GetTicks() < end;) {
        if (!platform_pump()) break;
        platform_set_fps(-1);
        SDL_DelayNS(5000000);
        SDL_LockMutex(elock);
        int pending = 0;
        for (int i = 0; i < n && !won; i++) {
            if (!tries[i]) continue;
            if (tries[i]->state == ENET_PEER_STATE_CONNECTED) won = tries[i];
            else if (tries[i]->state != ENET_PEER_STATE_DISCONNECTED) pending = 1;
        }
        SDL_UnlockMutex(elock);
        if (!won && !pending) break;
    }
    SDL_LockMutex(elock);
    for (int i = 0; i < n; i++)
        if (tries[i] && tries[i] != won) enet_peer_disconnect_now(tries[i], 0);
    SDL_UnlockMutex(elock);
    if (won) {
        char s[64];
        addr_str(&won->address, s, sizeof s);
        fprintf(stderr, "[net] connected to the host at %s\n", s);
    }
    return won;
}

static int join_start(const char* disc_root, const char* where) {
    if (!open_host(0)) { fail("Couldn't start networking on this PC."); return 0; }
    ENetAddress eps[MAX_EPS];
    int neps = 0;
    if (room[0]) {
        say("Online: looking for room %s...", room);
        JoinBoard* j = calloc(1, sizeof *j);
        SDL_DetachThread(SDL_CreateThread(join_board_thread, "room", j));
        while (!j->done) {
            if (!platform_pump()) return 0;
            platform_set_fps(-1);
            SDL_DelayNS(20000000);
        }
        if (!j->host.found) {
            fail(j->board_ok ? "There's no room with that code. Check the code, and that the host has pressed Host first."
                             : "Couldn't reach the room service (ntfy.sh). Check your internet connection, or join by IP address.");
            return 0;
        }
        fprintf(stderr, "[net] room %s: the host is at %s\n", room, j->host.eps);
        neps = parse_endpoints(j->host.eps, eps, MAX_EPS);
        say("Online: connecting to room %s...", room);
    } else {
        say("Online: connecting to %s...", where);
        if (parse_addr(where, &eps[0], DEFAULT_PORT)) neps = 1;
        else { fail("Couldn't find that address. Check what you typed."); return 0; }
    }
    ENetPeer* ep = connect_any(eps, neps, 20000);
    if (!ep) {
        fail(room[0] ? "Couldn't connect to the host. Your routers may block it: try again, or see the Online tab "
                       "in the launcher for other ways (a forwarded port or a VPN like Radmin VPN)."
                     : "Couldn't reach the host. Check the address, and that the host has pressed Host. "
                       "If it still doesn't work, see the Online tab in the launcher.");
        return 0;
    }
    Peer* pe = &peers[0];
    SDL_LockMutex(elock);
    pe->ep = ep;
    pe->alive = 1;
    ep->data = pe;
    SDL_UnlockMutex(elock);
    snprintf(pe->name, sizeof pe->name, "the host");
    uint8_t hello[17 + 32] = {0};
    uint32_t v[4] = {NET_MAGIC, NET_VERSION, build_id(), disc_id(disc_root)};
    memcpy(hello, v, 16);
    hello[16] = (uint8_t)nlocal_devs;
    const char* myname = getenv("WPR_NET_NAME");
    snprintf((char*)hello + 17, 32, "%s", myname && *myname ? myname : "Friend");
    send_msg(pe, M_HELLO, hello, sizeof hello);
    say("Online: connected, waiting for the host to start...");

    char nand[1100];
    nand_root_of(disc_root, nand, sizeof nand);
    strncat(nand, "-online", sizeof nand - strlen(nand) - 1);  /* the host's saves, kept apart from ours */
    SDL_CreateDirectory(nand);
    clear_dir(nand);
    for (;;) {
        int type;
        uint8_t* d;
        uint32_t len;
        int r = recv_msg(pe, 1 << 30, &type, &d, &len);
        if (r == -2) return 0;
        if (r != 1) { fail("The host closed the connection."); return 0; }
        if (type == M_REJECT) { fail((const char*)d); free(d); return 0; }
        if (type == M_PING) send_msg(pe, M_PONG, d, len);
        if (type == M_DIR && len < 1000 && strlen((char*)d) == len && !strstr((char*)d, "..") && d[0] != '/' &&
            d[0] != '\\' && !strchr((char*)d, ':')) {
            char path[2200];
            snprintf(path, sizeof path, "%s/%s", nand, (char*)d);
            SDL_CreateDirectory(path);
        }
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
    SDL_LockMutex(elock);
    start_dispatching();
    SDL_UnlockMutex(elock);
    fprintf(stderr, "[net] joined as peer %d, delay %d frames\n", me, delay);
    return 1;
}

int net_start(const char* disc_root) {
    const char* host = getenv("WPR_NET_HOST");
    const char* join = getenv("WPR_NET_JOIN");
    if (!(host && *host) && !(join && *join)) return 1;
    if (enet_initialize() != 0) { fprintf(stderr, "[net] enet_initialize failed\n"); return 0; }
    srand((unsigned)SDL_GetTicksNS());
    lock = SDL_CreateMutex();
    cond = SDL_CreateCondition();
    elock = SDL_CreateMutex();
    olock = SDL_CreateMutex();
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
        const char* code = getenv("WPR_NET_ROOM");  /* a room code (from the launcher): friends join with it */
        if (code && *code && as_room_code(code, room, sizeof room)) room_topic(room);
        mode = 1;
        ok = host_start(disc_root, atoi(host) > 0 ? atoi(host) : DEFAULT_PORT, friends);
    } else {
        if (as_room_code(join, room, sizeof room)) room_topic(room);
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
    say("Online: %d players", nplayers);
    return 1;
}
