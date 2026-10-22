/* Netplay: lockstep inputs over ENet/UDP, joined by room code or address (netplay.c). */
#pragma once
#include <stdint.h>
#include "runtime.h"

int net_active(void);
/* Before boot: WPR_NET_HOST=<port> waits for WPR_NET_PLAYERS friends, WPR_NET_JOIN=<room code or host[:port]>
 * connects and receives the host's save data and settings. Returns 0 if netplay was asked for and
 * failed (the reason is shown and logged), 1 otherwise. */
int net_start(const char* disc_root);
void net_frame(uint64_t frame);                  /* each VI: send our input, wait for this frame's */
void net_hash(uint64_t frame, uint64_t hash);    /* compare game state between players */
const HostRemote* net_remote(int player);        /* this frame's input for player 0..3 */
const char* net_status(void);                    /* for the window title, or NULL */
