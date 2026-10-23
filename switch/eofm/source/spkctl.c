#include "spkctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQ_PATH    "sdmc:/switch/usbhs-logmitm/speaker.txt"
#define STATUS_PATH "sdmc:/switch/usbhs-logmitm/speaker_status.txt"

bool spkctl_request(bool on, int gain) {
    if (!on) return remove(REQ_PATH) == 0;
    FILE *f = fopen(REQ_PATH, "w");
    if (!f) return false;
    fprintf(f, "on %d\n", gain);
    fclose(f);
    return true;
}

spk_state spkctl_status(char *why, size_t size) {
    char buf[128] = {0};
    if (why && size) why[0] = 0;
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) return SPK_NO_SERVICE;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    if (!strncmp(buf, "running", 7)) return SPK_RUNNING;
    if (!strncmp(buf, "error", 5)) {
        if (why && size) snprintf(why, size, "%s", buf[5] ? buf + 6 : "");
        return SPK_ERROR;
    }
    return SPK_OFF;
}
