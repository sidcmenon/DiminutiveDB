#include "crashpoint.h"
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned crash_at;
static unsigned crash_seen;
void khb_crash_init(void){
    const char *s = getenv("KHB_CRASH_AT");
    crash_at = (s != NULL) ? (unsigned)strtoul(s, NULL, 10) : 0u;
    crash_seen = 0;
}

void khb_crashpoint(void){
    crash_seen++;
    if (crash_at != 0 && crash_seen == crash_at){
        raise (SIGKILL);
        _exit(99);
    }

}

unsigned khb_crashpoint_total(void){
    return crash_seen;
}