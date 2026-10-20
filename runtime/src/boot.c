/* Boot: load the user's extracted disc, place the DOL in guest memory, and set up the low-memory
 * globals the Wii OS expects, the way Dolphin's BS2 HLE does (Source/Core/Core/Boot/Boot_BS2Emu.cpp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc_context.h"
#include "runtime.h"

#define GUEST_MEM_SIZE 0x14000000u /* covers MEM1 (phys 0x0) and MEM2 (phys 0x10000000) */

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int boot_load(const char* disc_root, uint32_t* entry_out) {
    char path[1024];
    size_t size;

    g_mem = calloc(1, GUEST_MEM_SIZE);
    if (!g_mem) { fprintf(stderr, "[boot] out of memory\n"); return 0; }

    snprintf(path, sizeof path, "%s/sys/main.dol", disc_root);
    uint8_t* dol = read_file(path, &size);
    if (!dol) { fprintf(stderr, "[boot] can't read %s (extract the disc with Dolphin first)\n", path); return 0; }
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(dol + i * 4), addr = be32(dol + 0x48 + i * 4), len = be32(dol + 0x90 + i * 4);
        if (len && off + len <= size) memcpy(g_mem + PPC_PHYS(addr), dol + off, len);
    }
    uint32_t bss = be32(dol + 0xD8), bss_len = be32(dol + 0xDC);
    memset(g_mem + PPC_PHYS(bss), 0, bss_len);
    *entry_out = be32(dol + 0xE0);
    free(dol);

    /* Disc header (game ID, maker, disc number) goes to 0x80000000. */
    snprintf(path, sizeof path, "%s/sys/boot.bin", disc_root);
    uint8_t* hdr = read_file(path, &size);
    if (hdr) { memcpy(g_mem, hdr, size < 0x20 ? size : 0x20); free(hdr); }

    /* Low-memory globals, values from Dolphin's SetupWiiMemory(). */
    mem_w32(0x80000020, 0x0D15EA5E);  /* boot magic */
    mem_w32(0x80000024, 0x00000001);  /* version */
    mem_w32(0x80000028, 0x01800000);  /* MEM1 size */
    mem_w32(0x8000002C, 0x00000023);  /* console type: retail Wii */
    mem_w32(0x800000F0, 0x01800000);  /* simulated memory size */
    mem_w32(0x800000F8, 0x0E7BE2C0);  /* bus clock 243 MHz / ... */
    mem_w32(0x800000FC, 0x2B73A840);  /* CPU clock 729 MHz */
    mem_w32(0x80003100, 0x01800000);  /* physical MEM1 size */
    mem_w32(0x80003104, 0x01800000);
    mem_w32(0x80003108, 0x01800000);
    mem_w32(0x8000310C, 0x81800000);  /* MEM1 arena end */
    mem_w32(0x80003118, 0x04000000);  /* MEM2 size */
    mem_w32(0x8000311C, 0x04000000);
    mem_w32(0x80003120, 0x93400000);  /* MEM2 end */
    mem_w32(0x80003124, 0x90000800);  /* usable MEM2 start */
    mem_w32(0x80003128, 0x933E0000);  /* usable MEM2 end */
    mem_w32(0x80003130, 0x933E0000);  /* IOS heap start */
    mem_w32(0x80003134, 0x93400000);  /* IOS heap end */
    mem_w32(0x80003138, 0x00000011);  /* hollywood revision */
    mem_w32(0x80003140, 0x00090204);  /* IOS version (IOS9 placeholder; real one comes from TMD) */
    mem_w32(0x80003188, 0x00090204);
    fprintf(stderr, "[boot] loaded %s, entry %08X\n", disc_root, *entry_out);
    return 1;
}
