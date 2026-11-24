#!/usr/bin/env python3
"""Other hosts for the tests of plugins' sockets (tests/fake_plugins_net.py), as honest as a PC makes them: a network
namespace of their own behind a veth pair, so that what hassmic connects to leaves its namespace over an interface and
is no address of the Echo's (plugin_net.c refuses those, and an address a "local" route delivers over lo too: the setup
before this one was exactly that, a review of 08399dc).

    peerhost.py <socket path>          (started by tests/run.py netns(peer=True), in the outer user namespace)

It makes the namespace (unshare), says so (<path>.ns), waits for the shell to have moved the veth's other end into it
(<path>.link), puts the hosts' addresses on it: 10.250.0.2/24 (a private network; the Echo's side is 10.250.0.1) and
198.51.100.7/24 (TEST-NET-2, outside the local network, routed by the Echo's side through 10.250.0.2), 10.250.0.3 for a
server that never answers (the test fills its queue), and then makes
sockets in there for the test on its asking over the Unix socket <path>: a line "udp <host> <port>" or "tcp <host>
<port> <backlog>", answered with the socket's descriptor (SCM_RIGHTS), bound (and listening).  The test uses them as
its own; they stay in this namespace.  It ends with the process that started it (PR_SET_PDEATHSIG)."""
import ctypes, os, signal, socket, subprocess, sys, time


def main(path):
    ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL)        # PR_SET_PDEATHSIG: gone with the test
    os.unshare(os.CLONE_NEWNET)
    open(path + ".ns", "w").close()
    for _ in range(500):
        if os.path.exists(path + ".link"): break
        time.sleep(0.02)
    for c in ("ip link set lo up", "ip addr add 10.250.0.2/24 dev v1", "ip addr add 10.250.0.3/24 dev v1", "ip addr add 198.51.100.7/24 dev v1", "ip link set v1 up",
              "ip route add default via 10.250.0.1"):
        subprocess.run(c.split(), check=True)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(path); srv.listen(8)
    while True:
        c, _ = srv.accept()
        try:
            f = c.makefile("r")
            for line in f:
                w = line.split()
                if not w: continue
                if w[0] == "udp": s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                else: s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((w[1], int(w[2])))
                if w[0] == "tcp": s.listen(int(w[3]) if len(w) > 3 else 4)
                socket.send_fds(c, [b"s"], [s.fileno()]); s.close()
        except OSError:
            pass
        finally:
            c.close()


def shell(path, py):
    """the outer shell's part (tests/run.py netns): start it, move the veth's end into its namespace, route"""
    q = f"'{path}'"
    return (f"{{ {py} {os.path.abspath(__file__)} {q} & }} && P=$! && "        # ({ ... & }: $! is the helper's own pid
            f"i=0; until [ -e {q}.ns ] || [ $i -gt 500 ]; do sleep 0.02; i=$((i+1)); done && "
            "ip link add v0 type veth peer name v1 netns $P && ip addr add 10.250.0.1/24 dev v0 && ip link set v0 up && "
            "ip route add 198.51.100.0/24 via 10.250.0.2 && "
            # an address no interface has, delivered to this host itself over lo: what plugin_net.c must refuse
            "ip route add local 10.251.0.0/24 dev lo && "
            f"touch {q}.link && i=0; until [ -S {q} ] || [ $i -gt 500 ]; do sleep 0.02; i=$((i+1)); done && "
            f"export HASSMIC_PEER_SOCK={q} && ")


def peer_socket(kind, host, port, backlog=4):
    """a socket made in the other hosts' namespace (HASSMIC_PEER_SOCK)"""
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); c.connect(os.environ["HASSMIC_PEER_SOCK"])
    c.sendall(f"{kind} {host} {port} {backlog}\n".encode())
    _, fds, _, _ = socket.recv_fds(c, 16, 1); c.close()
    if not fds: raise OSError(f"the other hosts made no {kind} socket on {host}:{port}")
    return socket.socket(fileno=fds[0])


if __name__ == "__main__":
    main(sys.argv[1])
