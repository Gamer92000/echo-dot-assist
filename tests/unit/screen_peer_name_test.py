#!/usr/bin/env python3
"""Exercise the screen's public hello parser without a physical Show or network."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "src/hassmic/screen.c").read_text()
parser = source[source.index("static int peer_hello_name("):source.index("/* Network reads never block")]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "json.h"
static const char *reply;
static int net_connect(const char *host, const char *port, int timeout) {
    (void)host; (void)port; (void)timeout;
    int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    assert(write(fd[1], reply, strlen(reply)) == (ssize_t)strlen(reply));
    shutdown(fd[1], SHUT_WR);
    /* The parser sends only a small request. Keep its socket alive until process exit. */
    return fd[0];
}
static long long mono_ms(void) { return 0; }
static void clean_cut(char *out, size_t cap, const char *in) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) out[n++] = (unsigned char)*in < 32 ? ' ' : *in;
    out[n] = 0;
}
'''
checks = r'''
int main(void) {
    char name[64];
    reply = "HTTP/1.0 200 OK\r\n\r\n{\"node\":\"peer\",\"name\":\"Echo Séjour <&>\"}";
    assert(!peer_hello_name("127.0.0.1", 28931, "peer", name));
    assert(!strcmp(name, "Echo Séjour <&>"));
    assert(peer_hello_name("127.0.0.1", 28931, "other", name) == -1);
    const char *bad[] = {
        "HTTP/1.0 403 Forbidden\r\n\r\n{}",
        "HTTP/1.0 200 OK\r\n\r\n{",
        "HTTP/1.0 200 OK\r\n\r\n{\"node\":\"peer\",\"name\":\"\"}",
        "HTTP/1.0 200 OK\r\n\r\n{\"node\":\"peer\",\"name\":42}",
        "HTTP/1.0 200 OK\r\n\r\n{\"name\":\"Wrong Echo\"}"
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reply = bad[i]; assert(peer_hello_name("127.0.0.1", 28931, "peer", name) == -1);
    }
    puts("screen peer names: UTF-8, node match and invalid response fallback ok");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp)
    (path / "test.c").write_text(harness + parser + checks)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/hassmic"),
                    str(path / "test.c"), str(ROOT / "src/hassmic/json.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=10)