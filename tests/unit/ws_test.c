#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "netio.h"
#include "ws.h"
/* ws_test server PORT: accept one client, echo every message back (one over 4 MB: "over <opcode> <length>", then what was
 * kept of it); "listening" on stdout once bound; exit 0 once the connection closed, 3 when ws_recv gave -1.  ws_test client PORT: connect, send text +
 * 70000 byte binary, check echo. */
int main(int c, char **v) {
    struct ws w; int op, r; uint8_t *d; size_t n; int port = atoi(v[2]);
    if (!strcmp(v[1], "server")) {
        int ls = net_listen(port); puts("listening"); fflush(stdout); int fd = net_accept(ls); char path[64];
        if (ws_accept(&w, fd, path, sizeof path)) return 1;
        fprintf(stderr, "server: path %s\n", path);
        while ((r = ws_recv(&w, &op, &d, &n)) >= 1) {
            if (r == 2) { char t[48]; ws_send(&w, WS_TEXT, t, snprintf(t, sizeof t, "over %d %zu", op, n)); }
            ws_send(&w, op, d, n);
        }
        free(w.buf);                                    /* not ws_close: after a -1 nothing may be answered */
        return r < 0 ? 3 : 0;
    }
    if (ws_connect(&w, "127.0.0.1", port, "/sendspin")) { puts("connect failed"); return 1; }
    static uint8_t big[70000]; for (size_t i = 0; i < sizeof big; i++) big[i] = i * 7;
    ws_send(&w, WS_TEXT, "hello", 5);
    if (ws_recv(&w, &op, &d, &n) != 1 || op != WS_TEXT || n != 5 || memcmp(d, "hello", 5)) { puts("text echo FAIL"); return 1; }
    ws_send(&w, WS_BINARY, big, sizeof big);
    if (ws_recv(&w, &op, &d, &n) != 1 || op != WS_BINARY || n != sizeof big || memcmp(d, big, n)) { puts("binary echo FAIL"); return 1; }
    ws_close(&w); puts("own client <-> own server ok");
    return 0;
}
