#include <cbm.h>
#include <string.h>
#include "config.h"

/* Config file on device 8 (floppy/SD) */
#define CFG_DEVICE  8
#define CFG_LFN     2

uint8_t config_load(char *host) {
    int n;

    if (cbm_open(CFG_LFN, CFG_DEVICE, CBM_READ, "meshconf,s,r")) {
        return 0;  /* file not found or device not present */
    }

    n = cbm_read(CFG_LFN, host, CONFIG_MAX_HOST - 1);
    cbm_close(CFG_LFN);

    if (n <= 0) {
        host[0] = '\0';
        return 0;
    }

    host[n] = '\0';

    /* Strip any trailing CR/LF */
    while (n > 0 && (host[n - 1] == '\r' || host[n - 1] == '\n' ||
                      host[n - 1] == '\0')) {
        host[--n] = '\0';
    }

    return (n > 0) ? 1 : 0;
}

uint8_t config_save(const char *host) {
    uint8_t len;

    /* Delete existing file first (CBM requires this for overwrite) */
    cbm_open(CFG_LFN, CFG_DEVICE, 15, "s:meshconf");
    cbm_close(CFG_LFN);

    if (cbm_open(CFG_LFN, CFG_DEVICE, CBM_WRITE, "meshconf,s,w")) {
        return 0;  /* device not present or disk error */
    }

    len = (uint8_t)strlen(host);
    cbm_write(CFG_LFN, host, len);
    cbm_close(CFG_LFN);

    return 1;
}
