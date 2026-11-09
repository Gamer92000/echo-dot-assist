import asyncio, struct, sys, aiohttp
from aiohttp import web
def hdr(op, n, fin=1, mask=b"\0\0\0\0"):                    # a client's frame header, its mask key included
    return bytes([fin << 7 | op]) + (bytes([0x80 | n]) if n < 126 else struct.pack(">BH", 0x80 | 126, n) if n < 65536
                                     else struct.pack(">BQ", 0x80 | 127, n)) + mask
def frame(op, data, fin=1, mask=b"\x9e\x31\x07\xc4"):        # a client's whole frame, its payload masked
    m = (mask * (len(data) // 4 + 1))[:len(data)]
    return hdr(op, len(data), fin, mask) + (int.from_bytes(data, "big") ^ int.from_bytes(m, "big")).to_bytes(len(data), "big")
async def recv(r):                                           # one frame of our server's: whole (FIN) and never masked
    b = await r.readexactly(2); n = b[1]
    assert b[0] & 0x80 and n < 0x80, b
    if n == 126: n, = struct.unpack(">H", await r.readexactly(2))
    elif n == 127: n, = struct.unpack(">Q", await r.readexactly(8))
    return b[0] & 0x0f, await r.readexactly(n)
async def server(port):                                      # ws_test's server, once it listens (a probe would take its one accept)
    p = await asyncio.create_subprocess_exec(sys.argv[1], "server", str(port), stdout=asyncio.subprocess.PIPE)
    assert await asyncio.wait_for(p.stdout.readline(), 5) == b"listening\n"; return p
async def upgrade(port):                                     # by hand, with RFC 6455's example key (and its answer)
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(b"GET /raw HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
    h = await r.readuntil(b"\r\n\r\n"); assert b" 101 " in h and b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in h, h
    return r, w
async def main():
    # 0) our server refuses a browser's upgrade (it carries Origin)
    p = await server(16961)
    async with aiohttp.ClientSession() as s:
        try:
            async with s.ws_connect("ws://127.0.0.1:16961/x", origin="http://example.com"): raise AssertionError("upgrade with Origin accepted")
        except aiohttp.WSServerHandshakeError as e: assert e.status == 403, e.status
    assert await p.wait() == 1; print("our server refuses an upgrade with Origin: ok")
    # 1) our server, aiohttp client
    p = await server(16961)
    async with aiohttp.ClientSession() as s, s.ws_connect("ws://127.0.0.1:16961/x") as ws:
        await ws.send_str("héllo"); m = await ws.receive(); assert m.type == aiohttp.WSMsgType.TEXT and m.data == "héllo"
        await ws.send_bytes(bytes(range(256)) * 300); m = await ws.receive(); assert m.data == bytes(range(256)) * 300
        await ws.ping(); await ws.close()
    await p.wait(); print("aiohttp client <-> our server ok")
    # 2) aiohttp server, our client
    async def h(req):
        ws = web.WebSocketResponse(); await ws.prepare(req)
        async for m in ws:
            if m.type == aiohttp.WSMsgType.TEXT: await ws.send_str(m.data)
            elif m.type == aiohttp.WSMsgType.BINARY: await ws.send_bytes(m.data)
        return ws
    app = web.Application(); app.router.add_get("/sendspin", h); r = web.AppRunner(app); await r.setup()
    await web.TCPSite(r, "127.0.0.1", 16962).start()
    p = await asyncio.create_subprocess_exec(sys.argv[1], "client", "16962", stdout=asyncio.subprocess.PIPE); out, _ = await p.communicate()
    print("our client <-> aiohttp server:", out.decode().strip()); await r.cleanup()
    # 3) our server, a client's frames written by hand: aiohttp never fragments (3.14.3 sets FIN on every frame it
    #    writes), RFC 6455 lets any other peer (5.4)
    p = await server(16961)
    r, w = await upgrade(16961)
    w.write(frame(1, b"h\xc3", 0) + frame(0, b"\xa9llo, w\xc3", 0) + frame(0, b"\xb6rld"))      # cut inside é and ö
    assert await recv(r) == (1, "héllo, wörld".encode()); print("a text in 3 fragments, echoed whole: ok")
    big = bytes(range(256)) * 300                                # fragments of 70000, 300 and 6500 bytes: 64- and 16-bit lengths
    w.write(frame(2, big[:70000], 0) + frame(9, b"ping 1") + frame(0, big[70000:70300], 0) + frame(10, b"unasked") + frame(0, big[70300:]))
    assert await recv(r) == (10, b"ping 1") and await recv(r) == (2, big)
    print("a ping and an unasked pong between fragments: the ping answered at once, the message whole after it: ok")
    # over 4 MB: read to its end, its first 256 bytes kept (100 of the first fragment, 156 of the second under another
    # mask), the first fragment's opcode, a ping in its middle answered, the connection usable after it
    msg = bytes(range(251)) * 16800                              # 4 216 800 bytes; a period no mask key or 256 lines up with
    w.write(frame(2, msg[:100], 0) + frame(0, msg[100:4200000], 0, b"\x01\x02\x03\x04") + frame(9, b"ping 2") + frame(0, msg[4200000:]))
    assert await recv(r) == (10, b"ping 2") and await recv(r) == (1, b"over 2 256") and await recv(r) == (2, msg[:256])
    w.write(frame(1, b"after")); assert await recv(r) == (1, b"after")
    print("4.2 MB in fragments, a ping among them: return 2 with its first 256 bytes, the next message as usual: ok")
    # crossing 4 MB only in its second fragment, after 3 MB: the same 256 bytes, not all that came before
    w.write(frame(2, msg[:3000000], 0) + frame(0, msg[3000000:]))
    assert await recv(r) == (1, b"over 2 256") and await recv(r) == (2, msg[:256])
    print("4.2 MB crossing 4 MB in its second fragment: its first 256 bytes too: ok")
    # the 64 MB refusal weighs one frame (and what is kept), not the message: 68 MB in 17 frames of 4 MB is read to its
    # end like 4.2 MB, only its head in memory (zero mask keys: the payload as written)
    w.write(frame(2, msg[:100], 0))
    for _ in range(17):
        w.write(hdr(0, 4 << 20, 0))
        for _ in range(4): w.write(bytes(1 << 20)); await w.drain()
    w.write(frame(0, b"")); assert await recv(r) == (1, b"over 2 256") and await recv(r) == (2, msg[:100] + bytes(156))
    print("68 MB in 17 frames of 4 MB: return 2 too (the 64 MB limit is a frame's): ok")
    w.write(frame(1, b"half", 0) + frame(8, b"\x03\xe8bye"))
    assert await recv(r) == (8, b"\x03\xe8") and await p.wait() == 0; print("a close between fragments: its code echoed, return 0: ok")
    # 4) frames that end the connection (return -1: ws_test exits 3) before their payload is read, a server each
    for what, f in (("a ping of 126 bytes", hdr(9, 126, 1, b"\x9e\x31\x07\xc4")), ("a frame of 64 MB + 1", hdr(2, (64 << 20) + 1))):
        p = await server(16961)
        r, w = await upgrade(16961); w.write(f)
        assert await asyncio.wait_for(p.wait(), 5) == 3 and await r.read() == b"", what
        w.close(); print(f"{what}: return -1, nothing answered: ok")
asyncio.run(main())
