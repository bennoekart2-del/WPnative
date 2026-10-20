/* IOS devices: ES (titles), FS + NAND files, STM (power/state), DI (the disc, served from the
 * extracted folder). Command numbers and behaviour follow Dolphin's Source/Core/Core/IOS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ios.h"
#include "ppc_context.h"

static char disc_root[1024];
static char nand_root[1100];

static uint32_t be32p(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void zero_out(uint32_t addr, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) mem_w8(addr + i, 0);
}

/* ------------------------------------------------------------------ disc layout from fst.bin */
typedef struct { uint64_t off, size; char path[512]; } DiscFile;
static DiscFile* files;
static int n_files;
static uint32_t game_id_hi;

static void add_file(uint64_t off, uint64_t size, const char* rel) {
    files = realloc(files, sizeof *files * (size_t)(n_files + 1));
    files[n_files].off = off;
    files[n_files].size = size;
    snprintf(files[n_files].path, sizeof files[n_files].path, "%s/%s", disc_root, rel);
    n_files++;
}

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(*n);
    if (b && fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void build_disc_map(void) {
    char p[1100];
    size_t n;
    snprintf(p, sizeof p, "%s/sys/boot.bin", disc_root);
    uint8_t* boot = slurp(p, &n);
    if (!boot) return;
    game_id_hi = be32p(boot);
    uint64_t dol_off = (uint64_t)be32p(boot + 0x420) << 2, fst_off = (uint64_t)be32p(boot + 0x424) << 2;
    uint64_t fst_size = (uint64_t)be32p(boot + 0x428) << 2;
    free(boot);
    add_file(0, 0x440, "sys/boot.bin");
    add_file(0x440, 0x2000, "sys/bi2.bin");
    add_file(0x2440, 0, "sys/apploader.img");
    add_file(dol_off, 0, "sys/main.dol");
    add_file(fst_off, fst_size, "sys/fst.bin");

    snprintf(p, sizeof p, "%s/sys/fst.bin", disc_root);
    uint8_t* fst = slurp(p, &n);
    if (!fst) return;
    uint32_t count = be32p(fst + 8);
    const char* names = (const char*)fst + count * 12;
    /* walk the tree keeping a directory stack to build relative paths */
    uint32_t dir_end[64]; char dir_path[64][512]; int depth = 0;
    dir_end[0] = count; strcpy(dir_path[0], "files");
    for (uint32_t i = 1; i < count; i++) {
        while (depth > 0 && i >= dir_end[depth]) depth--;
        const uint8_t* e = fst + i * 12;
        const char* name = names + (be32p(e) & 0xFFFFFF);
        char rel[600];
        snprintf(rel, sizeof rel, "%s/%s", dir_path[depth], name);
        if (e[0]) {  /* directory: next field is the index after its last child */
            depth++;
            dir_end[depth] = be32p(e + 8);
            snprintf(dir_path[depth], sizeof dir_path[depth], "%s", rel);
        } else {
            add_file((uint64_t)be32p(e + 4) << 2, be32p(e + 8), rel);
        }
    }
    free(fst);
    fprintf(stderr, "[di] disc map: %d files\n", n_files);
}

static void disc_read(uint32_t dst, uint32_t len, uint64_t off) {
    zero_out(dst, len);
    for (int i = 0; i < n_files; i++) {
        DiscFile* f = &files[i];
        uint64_t size = f->size;
        if (!size) { struct stat st; if (stat(f->path, &st) == 0) size = f->size = (uint64_t)st.st_size; }
        if (off + len <= f->off || off >= f->off + size) continue;
        FILE* h = fopen(f->path, "rb");
        if (!h) { fprintf(stderr, "[di] missing file %s\n", f->path); continue; }
        if (getenv("WPR_TRACE_DISC")) fprintf(stderr, "[di] read %s +%llX len %X\n", f->path, (unsigned long long)(off > f->off ? off - f->off : 0), len);
        uint64_t lo = off > f->off ? off : f->off, hi = (off + len < f->off + size) ? off + len : f->off + size;
        fseek(h, (long)(lo - f->off), SEEK_SET);
        size_t got = fread(g_mem + PPC_PHYS(dst + (uint32_t)(lo - off)), 1, (size_t)(hi - lo), h);
        (void)got;
        fclose(h);
    }
}

/* ------------------------------------------------------------------ /dev/di */
static uint32_t di_last_error;

static int32_t di_ioctl(void* st, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len) {
    (void)st; (void)in_len;
    uint32_t a1 = mem_r32(in + 4), a2 = mem_r32(in + 8);
    switch (cmd) {
    case 0x8D: /* UnencryptedRead: system area only. The two probes past the end of a single-layer
                * disc must fail like a real drive does ("block out of range"), or the game shows
                * error #001. Ranges from Dolphin's DI.cpp. */
        if (a2 + (a1 >> 2) <= 0x14000) { disc_read(out, a1, (uint64_t)a2 << 2); return 1; }
        if ((a2 >= 0x460A0000 && a2 <= 0x460A0008) || (a2 >= 0x7ED40000 && a2 <= 0x7ED40008)) {
            di_last_error = 0x00052100;
            return 2;
        }
        return 32;
    case 0xA4: case 0xD0: case 0xD1: /* ReportKey / ReadDvd / ReadDvdConfig: retail drives reject */
        di_last_error = 0x00052000;
        return 2;
    case 0xE0: /* RequestError */
        mem_w32(out, di_last_error);
        di_last_error = 0;
        return 1;
    case 0x71: /* DVDLowRead: length, offset >> 2 */
        disc_read(out, a1, (uint64_t)a2 << 2);
        return 1;
    case 0x70: /* ReadDiskID */
        disc_read(out, 0x20, 0);
        return 1;
    case 0x88: /* GetCoverStatus: disc present, cover closed */
        mem_w32(out, 2);
        return 1;
    case 0x7A: case 0x95: case 0x96:
        zero_out(out, out_len);
        return 1;
    default:
        fprintf(stderr, "[di] ioctl %02X (%08X %08X) -> ok\n", cmd, a1, a2);
        if (out && out_len) zero_out(out, out_len);
        return 1;
    }
}

static int32_t di_ioctlv(void* st, uint32_t cmd, uint32_t n_in, uint32_t n_io, IosVec* v) {
    (void)st; (void)n_in; (void)n_io;
    if (cmd == 0x8B) { /* OpenPartition: out[0] = TMD buffer, out[1] = ES error */
        char p[1100]; size_t n;
        snprintf(p, sizeof p, "%s/tmd.bin", disc_root);
        uint8_t* tmd = slurp(p, &n);
        if (tmd && n_io >= 1) {
            memcpy(g_mem + PPC_PHYS(v[n_in].addr), tmd, n < v[n_in].size ? n : v[n_in].size);
        }
        free(tmd);
        if (n_io >= 2) mem_w32(v[n_in + 1].addr, 0);
        return 1;
    }
    fprintf(stderr, "[di] ioctlv %02X\n", cmd);
    return 1;
}

/* ------------------------------------------------------------------ /dev/es */
static int32_t es_ioctlv(void* st, uint32_t cmd, uint32_t n_in, uint32_t n_io, IosVec* v) {
    (void)st;
    IosVec* out = &v[n_in];
    switch (cmd) {
    case 0x20: /* GetTitleID */
        mem_w32(out[0].addr, 0x00010000);
        mem_w32(out[0].addr + 4, game_id_hi);
        return 0;
    case 0x1D: { /* GetTitleDir */
        char dir[64];
        snprintf(dir, sizeof dir, "/title/%08x/%08x/data", 0x00010000u, game_id_hi);
        for (size_t i = 0; i <= strlen(dir); i++) mem_w8(out[0].addr + (uint32_t)i, (uint8_t)dir[i]);
        return 0;
    }
    case 0x07: /* GetDeviceID */
        mem_w32(out[0].addr, 0x0403AC68);
        return 0;
    case 0x16: case 0x12: case 0x14: /* consumption / view counts */
        if (n_io) mem_w32(out[0].addr, 0);
        return 0;
    default:
        fprintf(stderr, "[es] ioctlv %02X (in %u io %u)\n", cmd, n_in, n_io);
        for (uint32_t i = 0; i < n_io; i++) zero_out(out[i].addr, out[i].size);
        return 0;
    }
}

/* ------------------------------------------------------------------ /dev/fs and NAND files */
static void nand_path(char* out, size_t n, const char* path) { snprintf(out, n, "%s%s", nand_root, path); }

static void mkdirs(const char* path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0;
#ifdef _WIN32
            mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/'; }
}

static void read_guest_str(char* dst, size_t n, uint32_t addr) {
    for (size_t i = 0; i < n - 1; i++) { dst[i] = (char)mem_r8(addr + (uint32_t)i); if (!dst[i]) return; }
    dst[n - 1] = 0;
}

static int32_t fs_ioctl(void* st, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len) {
    (void)st; (void)in_len;
    char path[64], host[1200];
    switch (cmd) {
    case 9:  /* CreateFile: uid, gid, path[64], perms... */
    case 3: { /* CreateDir */
        read_guest_str(path, sizeof path, in + 6);
        nand_path(host, sizeof host, path);
        mkdirs(host);
        if (cmd == 3) {
#ifdef _WIN32
            mkdir(host);
#else
            mkdir(host, 0755);
#endif
        } else {
            FILE* f = fopen(host, "ab"); if (f) fclose(f);
        }
        return 0;
    }
    case 6: { /* GetAttr(path) -> uid, gid, path, perms */
        read_guest_str(path, sizeof path, in);
        nand_path(host, sizeof host, path);
        struct stat s;
        if (stat(host, &s) != 0) return -106;
        zero_out(out, out_len);
        mem_w32(out, 0x1000); mem_w16(out + 4, 0x3031);
        for (size_t i = 0; i < 64 && i <= strlen(path); i++) mem_w8(out + 6 + (uint32_t)i, (uint8_t)path[i]);
        mem_w8(out + 0x46, 3); mem_w8(out + 0x47, 3); mem_w8(out + 0x48, 3);
        return 0;
    }
    case 7: /* Delete */
        read_guest_str(path, sizeof path, in);
        nand_path(host, sizeof host, path);
        remove(host);
        return 0;
    case 8: { /* Rename(old[64], new[64]) */
        char p2[64], h2[1200];
        read_guest_str(path, sizeof path, in);
        read_guest_str(p2, sizeof p2, in + 64);
        nand_path(host, sizeof host, path); nand_path(h2, sizeof h2, p2);
        mkdirs(h2); remove(h2);
        return rename(host, h2) == 0 ? 0 : -106;
    }
    case 2: /* GetStats */
        zero_out(out, out_len);
        mem_w32(out, 0x4000); mem_w32(out + 4, 0x5DEC); mem_w32(out + 8, 0x1DD4);
        return 0;
    case 5: case 10: case 13: /* SetAttr, SetFileVerCtrl, Shutdown */
        return 0;
    default:
        fprintf(stderr, "[fs] ioctl %u\n", cmd);
        return 0;
    }
}

static int32_t fs_ioctlv(void* st, uint32_t cmd, uint32_t n_in, uint32_t n_io, IosVec* v) {
    (void)st; (void)n_io;
    char path[64];
    read_guest_str(path, sizeof path, v[0].addr);
    if (cmd == 12) { /* GetUsage(path) -> blocks, inodes */
        mem_w32(v[n_in].addr, 0); mem_w32(v[n_in + 1].addr, 0);
        return 0;
    }
    if (cmd == 4) { /* ReadDir: we report empty directories for now */
        if (n_in == 1) mem_w32(v[n_in].addr, 0); else mem_w32(v[n_in + 1].addr, 0);
        return 0;
    }
    fprintf(stderr, "[fs] ioctlv %u %s\n", cmd, path);
    return 0;
}

typedef struct { FILE* f; } NandFile;

static int32_t file_open(const char* path, uint32_t mode, void** state) {
    char host[1200];
    nand_path(host, sizeof host, path);
    FILE* f = fopen(host, mode & 2 ? "r+b" : "rb");
    if (!f) { fprintf(stderr, "[fs] %s: not found\n", path); return -106; }
    NandFile* nf = malloc(sizeof *nf);
    nf->f = f;
    *state = nf;
    return 0;
}
static void file_close(void* st) { NandFile* nf = st; fclose(nf->f); free(nf); }
static int32_t file_read(void* st, uint32_t buf, uint32_t len) {
    NandFile* nf = st;
    return (int32_t)fread(g_mem + PPC_PHYS(buf), 1, len, nf->f);
}
static int32_t file_write(void* st, uint32_t buf, uint32_t len) {
    NandFile* nf = st;
    int32_t n = (int32_t)fwrite(g_mem + PPC_PHYS(buf), 1, len, nf->f);
    fflush(nf->f);
    return n;
}
static int32_t file_seek(void* st, int32_t off, uint32_t whence) {
    NandFile* nf = st;
    if (fseek(nf->f, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET)) return -101;
    return (int32_t)ftell(nf->f);
}

/* ------------------------------------------------------------------ /dev/stm */
static int32_t stm_ioctl(void* st, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len) {
    (void)st; (void)in; (void)in_len;
    if (cmd == 0x1000) return IOS_NO_REPLY; /* eventhook: replies only on power/reset button press */
    if (out && out_len) zero_out(out, out_len);
    return 0;
}

/* ------------------------------------------------------------------ fallback: log and succeed */
static int32_t stub_open(const char* path, uint32_t mode, void** state) { (void)mode; *state = (void*)path; return 0; }
static int32_t stub_ioctl(void* st, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len) {
    (void)st; (void)in; (void)in_len;
    fprintf(stderr, "[ios] stub ioctl %X\n", cmd);
    if (out && out_len) zero_out(out, out_len);
    return 0;
}
static int32_t stub_ioctlv(void* st, uint32_t cmd, uint32_t n_in, uint32_t n_io, IosVec* v) {
    (void)st;
    fprintf(stderr, "[ios] stub ioctlv %X\n", cmd);
    for (uint32_t i = 0; i < n_io; i++) zero_out(v[n_in + i].addr, v[n_in + i].size);
    return 0;
}

static int32_t bt_open_fail(const char* path, uint32_t mode, void** state) {
    (void)path; (void)mode; (void)state;
    return -6;
}

static const IosDevice devices[] = {
    {"/dev/di", stub_open, NULL, NULL, NULL, NULL, di_ioctl, di_ioctlv},
    {"/dev/es", stub_open, NULL, NULL, NULL, NULL, stub_ioctl, es_ioctlv},
    {"/dev/fs", stub_open, NULL, NULL, NULL, NULL, fs_ioctl, fs_ioctlv},
    {"/dev/usb/oh1/57e/305", bt_open_fail, NULL, NULL, NULL, NULL, NULL, NULL}, /* Bluetooth: TODO */
    {"/dev/stm/*", stub_open, NULL, NULL, NULL, NULL, stm_ioctl, stub_ioctlv},
    {"/dev/*", stub_open, NULL, NULL, NULL, NULL, stub_ioctl, stub_ioctlv},
    {"/*", file_open, file_close, file_read, file_write, file_seek, NULL, NULL},
};

const IosDevice* ios_find_device(const char* path) {
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        const char* p = devices[i].path;
        size_t n = strlen(p);
        if (p[n - 1] == '*' ? strncmp(path, p, n - 1) == 0 : strcmp(path, p) == 0) return &devices[i];
    }
    return NULL;
}

void ios_set_disc_root(const char* root) {
    snprintf(disc_root, sizeof disc_root, "%s", root);
    const char* env = getenv("WPR_NAND");
    if (env) snprintf(nand_root, sizeof nand_root, "%s", env);
    else snprintf(nand_root, sizeof nand_root, "%s/../nand", root);
    build_disc_map();
}
