import asyncio, sys, aiohttp
from aiohttp import web
async def main():
    # 1) our server, aiohttp client
    p = await asyncio.create_subprocess_exec(sys.argv[1], "server", "16961"); await asyncio.sleep(0.3)
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
asyncio.run(main())
