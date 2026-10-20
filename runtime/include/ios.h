/* Emulated IOS devices (what the game reaches through IOS_Open / IOS_Ioctl...). */
#pragma once
#include <stdint.h>

enum { IPC_OPEN = 1, IPC_CLOSE, IPC_READ, IPC_WRITE, IPC_SEEK, IPC_IOCTL, IPC_IOCTLV, IPC_REPLY };
#define IOS_NO_REPLY ((int32_t)0x7FFFFFFF)

typedef struct { uint32_t addr, size; } IosVec;
typedef struct { uint32_t addr, cmd; int32_t fd; uint32_t arg[5]; } IosRequest;

typedef struct IosDevice {
    const char* path;      /* exact path, or prefix ending in '*' */
    int32_t (*open)(const char* path, uint32_t mode, void** state);
    void (*close)(void* state);
    int32_t (*read)(void* state, uint32_t buf, uint32_t len);
    int32_t (*write)(void* state, uint32_t buf, uint32_t len);
    int32_t (*seek)(void* state, int32_t off, uint32_t whence);
    int32_t (*ioctl)(void* state, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len);
    int32_t (*ioctlv)(void* state, uint32_t cmd, uint32_t n_in, uint32_t n_io, IosVec* v);
} IosDevice;

const IosDevice* ios_find_device(const char* path);
void ios_set_disc_root(const char* root);
void ios_reply_later(uint32_t req, int32_t result);
