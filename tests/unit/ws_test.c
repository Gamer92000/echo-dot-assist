#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "netio.h"
#include "ws.h"
/* ws_test server PORT: accept one client, echo every message back.  ws_test client PORT: connect, send text + 70000 byte binary, check echo. */
int main(int c, char **v) {
    struct ws w; int op; uint8_t *d; size_t n; int port = atoi(v[2]);
    if (!strcmp(v[1], "server")) {
        int ls = net_listen(port), fd = net_accept(ls); char path[64];
        if (ws_accept(&w, fd, path, sizeof path)) return 1;
        fprintf(stderr, "server: path %s\n", path);
        while (ws_recv(&w, &op, &d, &n) == 1) ws_send(&w, op, d, n);
        return 0;
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
