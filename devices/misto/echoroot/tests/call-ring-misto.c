#include "ring.h"
#include <unistd.h>
int main(void) {
    ring_start(0, 0);
    ring_set("call_incoming"); usleep(200000);
    ring_unset("call_incoming"); ring_set("call_connected"); usleep(200000);
    ring_unset("call_connected"); ring_set("call_connected_end"); usleep(200000);
    usleep(750000); ring_tick(); usleep(200000);
    return 0;
}
