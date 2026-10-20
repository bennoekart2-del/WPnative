/* Wii Remote input at the WPAD/WUD level. The real path is Bluetooth HCI through
 * /dev/usb/oh1/57e/305; until that is emulated, the Bluetooth stack is skipped. */
#include "ppc_context.h"

void hle_WUDInit(PPCContext* c) {
    c->r[3] = 0;
}
