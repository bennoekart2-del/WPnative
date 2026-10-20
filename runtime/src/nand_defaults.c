/* First-run system settings for the emulated NAND, like a freshly set up Wii: SYSCONF (console
 * settings: language, sensor bar, aspect ratio...) and setting.txt (region). Layouts and default
 * values follow Dolphin's Core/SysConf.cpp and Common/SettingsHandler.cpp.
 *
 * WPR_LANGUAGE picks the console language: en (default), de, fr, es, it, nl.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BIG_ARRAY = 1, SMALL_ARRAY, BYTE, SHORT, LONG, LONGLONG, BYTE_BOOL };

typedef struct { int type; const char* name; const uint8_t* data; int len; } Entry;

static void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int write_sysconf(const char* path, int lang, int country) {
    static uint8_t dinf[0x461], sadr[0x1007], pc[0x4A], nik[0x16], idl[2] = {0, 1};
    uint8_t b0 = 0, b1 = 1, b2 = 2, spkv = 0x58, lng = (uint8_t)lang;
    uint8_t sens[4] = {0, 0, 0, 3}, frc[4] = {0, 0, 0, 0x28}, zero4[4] = {0}, one4[4] = {0, 0, 0, 1};
    sadr[0] = (uint8_t)country;
    const char* nick = "Player";
    for (int i = 0; nick[i]; i++) nik[i * 2 + 1] = (uint8_t)nick[i];
    nik[0x14] = 0; nik[0x15] = (uint8_t)strlen(nick);
    const Entry e[] = {
        {BIG_ARRAY, "BT.DINF", dinf, sizeof dinf}, {LONG, "BT.SENS", sens, 4}, {BYTE, "BT.SPKV", &spkv, 1},
        {BYTE, "BT.MOT", &b1, 1}, {BYTE, "BT.BAR", &b0, 1},
        {BIG_ARRAY, "IPL.SADR", sadr, sizeof sadr}, {SMALL_ARRAY, "IPL.PC", pc, sizeof pc},
        {LONG, "IPL.CB", zero4, 4}, {BYTE, "IPL.UPT", &b2, 1}, {BYTE, "IPL.PGS", &b0, 1},
        {BYTE, "IPL.SSV", &b1, 1}, {BYTE, "IPL.LNG", &lng, 1}, {BYTE, "IPL.SND", &b1, 1},
        {BYTE, "IPL.AR", &b0, 1}, {BYTE, "IPL.DH", &b0, 1}, {BYTE, "IPL.E60", &b1, 1},
        {LONG, "IPL.INC", zero4, 4}, {LONG, "IPL.FRC", frc, 4}, {BYTE_BOOL, "IPL.CD", &b1, 1},
        {BYTE_BOOL, "IPL.CD2", &b1, 1}, {BYTE_BOOL, "IPL.EULA", &b1, 1}, {SMALL_ARRAY, "IPL.IDL", idl, 2},
        {SMALL_ARRAY, "IPL.NIK", nik, sizeof nik}, {LONG, "NET.CTPC", zero4, 4}, {LONG, "NET.WCFG", one4, 4},
        {BYTE, "DEV.BTM", &b0, 1}, {BYTE, "DEV.VIM", &b0, 1}, {BYTE, "DEV.CTC", &b1, 1},
        {BYTE, "DEV.DSM", &b1, 1}, {BYTE_BOOL, "MPLS.MOVIE", &b1, 1},
    };
    const int n = (int)(sizeof e / sizeof e[0]);
    static uint8_t buf[0x4000];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "SCv0", 4);
    put16(buf + 4, (unsigned)n);
    int pos = 6 + 2 * (n + 1);
    for (int i = 0; i < n; i++) {
        put16(buf + 6 + 2 * i, (unsigned)pos);
        int nl = (int)strlen(e[i].name);
        buf[pos++] = (uint8_t)(e[i].type << 5 | (nl - 1));
        memcpy(buf + pos, e[i].name, (size_t)nl);
        pos += nl;
        if (e[i].type == BIG_ARRAY) { put16(buf + pos, (unsigned)(e[i].len - 1)); pos += 2; }
        else if (e[i].type == SMALL_ARRAY) buf[pos++] = (uint8_t)(e[i].len - 1);
        memcpy(buf + pos, e[i].data, (size_t)e[i].len);
        pos += e[i].len;
        if (e[i].type == BIG_ARRAY || e[i].type == SMALL_ARRAY) pos++;  /* arrays carry a trailing NUL */
    }
    put16(buf + 6 + 2 * n, (unsigned)pos);
    memcpy(buf + sizeof buf - 4, "SCed", 4);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    return 1;
}

static int write_setting_txt(const char* path) {
    static const char text[] =
        "AREA=EUR\r\nMODEL=RVL-001(EUR)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LEH\r\n"
        "SERNO=123456789\r\nVIDEO=PAL\r\nGAME=EU\r\n";
    uint8_t buf[0x100] = {0};
    memcpy(buf, text, sizeof text - 1);
    uint32_t key = 0x73B5DBFA;
    for (int i = 0; i < 0x100; i++) {
        buf[i] ^= (uint8_t)key;
        key = (key << 1) | (key >> 31);
    }
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    return 1;
}

static int exists(const char* p) { FILE* f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

void nand_create_defaults(const char* root, void (*mkdirs)(const char*)) {
    static const struct { const char* code; int lang, country; } langs[] = {
        {"en", 1, 110}, {"de", 2, 78}, {"fr", 3, 77}, {"es", 4, 105}, {"it", 5, 83}, {"nl", 6, 94},
    };
    const char* want = getenv("WPR_LANGUAGE");
    int lang = 1, country = 110;
    for (size_t i = 0; want && i < sizeof langs / sizeof langs[0]; i++)
        if (strcmp(want, langs[i].code) == 0) { lang = langs[i].lang; country = langs[i].country; }
    char p[1200];
    snprintf(p, sizeof p, "%s/shared2/sys/SYSCONF", root);
    if (!exists(p)) { mkdirs(p); if (write_sysconf(p, lang, country)) fprintf(stderr, "[nand] created %s\n", p); }
    snprintf(p, sizeof p, "%s/title/00000001/00000002/data/setting.txt", root);
    if (!exists(p)) { mkdirs(p); if (write_setting_txt(p)) fprintf(stderr, "[nand] created %s\n", p); }
}
