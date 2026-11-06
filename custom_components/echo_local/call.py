"""Drop In from a Home Assistant dashboard to an Echo Show (docs/video-dropin.md): a websocket for the call card
(frontend/echo-local-call-card.js), relayed to the Echo's call channel (api.py Echo.drop_in, web.c /api/call).

    GET /api/echo_local/call/<camera entity id>      websocket; opened with a signed path (auth/sign_path), so only a
                                                      logged-in user of Home Assistant gets here, never the kiosk users
                                                      Echo Local makes for the Echos' screens

To the card:   text {"state": "ringing" | "connected" | "idle"} and {"error": "<the Echo's refusal>"}
               binary b"A" + 640 bytes (20 ms of 16 kHz s16le: the Echo's microphone, through Drop In's gate)
               binary b"V" + type u8 + pts u64 LE (us) + H.264 Annex B (camera.h's records: 1 config, 2 key, 3 frame)
From it:       binary b"A" + 640 bytes (its microphone, or silence: sent all along, the call's keepalive)
               binary b"V" + type u8 + pts u64 LE (us) + H.264 Annex B (its camera, WebCodecs; key frames with SPS/PPS)
                      for the Echo Show's screen (the camera entity's shows_video): passed on as it comes
               text {"hangup": true}

Video and sound go through as they were made, both ways; nothing is kept here."""
from __future__ import annotations

import asyncio
import json
import logging
import struct

from aiohttp import WSMsgType, web

from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er

from .api import EchoAuthError, EchoError
from .const import DOMAIN
from .kiosk import DATA_USER

_LOGGER = logging.getLogger(__name__)
CALL_PATH = "/api/echo_local/call/{entity_id}"


class CallView(HomeAssistantView):
    url = CALL_PATH
    name = "api:echo_local:call"
    requires_auth = True

    async def get(self, request: web.Request, entity_id: str) -> web.StreamResponse:
        hass: HomeAssistant = request.app["hass"]
        user = request["hass_user"]
        kiosk_users = {e.data.get(DATA_USER) for e in hass.config_entries.async_entries(DOMAIN)}
        if user.id in kiosk_users or user.system_generated:
            raise web.HTTPForbidden()               # an Echo's own screen does not drop in on Echos
        ent = er.async_get(hass).async_get(entity_id)
        entry = hass.config_entries.async_get_entry(ent.config_entry_id) if ent and ent.platform == DOMAIN else None
        if not entry or not getattr(entry, "runtime_data", None):
            raise web.HTTPNotFound()
        echo = entry.runtime_data.echo
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=1 << 20)
        await ws.prepare(request)
        try:
            call = await echo.drop_in()
        except EchoAuthError:
            await ws.send_str(json.dumps({"error": "Home Assistant is not let in on this Echo any more"}))
            entry.async_start_reauth(hass)
            await ws.close(); return ws
        except EchoError as e:
            await ws.send_str(json.dumps({"error": str(e)}))
            await ws.close(); return ws
        _LOGGER.info("%s: Drop In from %s", entry.title, user.name)

        async def down() -> None:                   # the Echo -> the card
            while (r := await call.recv()) is not None:
                if r[0] == "S":
                    await ws.send_str(json.dumps({"state": r[1]}))
                    if r[1] == "idle":
                        return
                elif r[0] == "A":
                    await ws.send_bytes(b"A" + r[1])
                elif r[0] == "V":
                    t, pts, data = r[1]
                    await ws.send_bytes(b"V" + bytes([t]) + struct.pack("<Q", pts) + data)

        async def up() -> None:                     # the card -> the Echo
            async for m in ws:
                if m.type == WSMsgType.BINARY and m.data[:1] == b"A" and len(m.data) == 1 + 640:
                    await call.audio(m.data[1:])
                elif m.type == WSMsgType.BINARY and m.data[:1] == b"V" and len(m.data) > 10:
                    await call.video(m.data[1], struct.unpack_from("<Q", m.data, 2)[0], m.data[10:])
                elif m.type == WSMsgType.TEXT:
                    try:
                        if json.loads(m.data).get("hangup"):
                            return
                    except ValueError:
                        pass

        tasks = [asyncio.ensure_future(down()), asyncio.ensure_future(up())]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        except (ConnectionError, EchoError) as e:
            _LOGGER.debug("%s: call: %s", entry.title, e)
        finally:
            for t in tasks:
                t.cancel()
            await call.hangup()
            if not ws.closed:
                try:
                    await ws.send_str(json.dumps({"state": "idle"}))
                except ConnectionError:
                    pass
                await ws.close()
            _LOGGER.info("%s: Drop In ended", entry.title)
        return ws


async def async_setup(hass: HomeAssistant) -> None:
    hass.http.register_view(CallView())
