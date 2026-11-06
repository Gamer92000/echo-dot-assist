"""The Echo Show's camera as a Home Assistant camera (docs/camera.md).

The Echo encodes H.264 in its hardware and sends it to this integration only, sealed with the key Home Assistant
shares with it (api.py camera_stream).  A relay here turns it into MPEG-TS (tsmux.py) on 127.0.0.1, which is the
entity's stream_source: Home Assistant's go2rtc makes WebRTC of it without transcoding, its stream integration HLS and
recordings, ffmpeg the stills.  One stream from the Echo, however many viewers: they share it, each from a key frame
(one a second).  The Echo's camera runs only while someone looks, plus a few seconds (LINGER_S), and shows a red dot
on its screen meanwhile.

Why not ESPHome's camera entity, which hassmic could have had without this file: MJPEG through Home Assistant's
Python, several times the Wi-Fi of H.264, no WebRTC, recordings re-encoded; docs/camera.md has the rest."""
from __future__ import annotations

import asyncio
import logging
import secrets

from aiohttp import web

from homeassistant.components.camera import Camera, CameraEntityFeature
from homeassistant.components.ffmpeg import async_get_image
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import EchoLocalCoordinator
from .api import CAM_CONFIG, CAM_KEY, EchoAuthError, EchoError
from .const import DOMAIN
from .switch import device_info
from .tsmux import TsMuxer

_LOGGER = logging.getLogger(__name__)
LINGER_S = 10          # the camera kept on after the last viewer: a dashboard's still every 10 s, a reload, a seek
QUEUE = 120            # access units a viewer may fall behind (8 s at 15 fps) before it is let go


class Relay:
    """127.0.0.1 only, one per Home Assistant: /<feed token>.ts.  Local processes are the only ones that reach it, and
    the token (a URL of its own per camera, made at each start) keeps those out that have not been handed the URL"""

    def __init__(self) -> None:
        self.feeds: dict[str, Feed] = {}
        self.port = 0
        self._runner: web.AppRunner | None = None

    async def start(self) -> None:
        if self._runner:
            return
        app = web.Application()
        app.router.add_get("/{token}.ts", self._serve)
        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, "127.0.0.1", 0)
        await site.start()
        self.port = site._server.sockets[0].getsockname()[1]

    async def _serve(self, request: web.Request) -> web.StreamResponse:
        feed = self.feeds.get(request.match_info["token"])
        if not feed:
            raise web.HTTPNotFound()
        return await feed.serve(request)

    def url(self, feed: "Feed") -> str:
        return f"http://127.0.0.1:{self.port}/{feed.token}.ts"


class Feed:
    """one Echo's stream, shared by its viewers"""

    def __init__(self, hass: HomeAssistant, coordinator: EchoLocalCoordinator, name: str) -> None:
        self.hass, self.c, self.name = hass, coordinator, name
        self.token = secrets.token_urlsafe(24)
        self.viewers: set[asyncio.Queue] = set()
        self.task: asyncio.Task | None = None
        self.config = b""
        self.error = ""
        self._stop: asyncio.TimerHandle | None = None

    async def serve(self, request: web.Request) -> web.StreamResponse:
        q: asyncio.Queue = asyncio.Queue(QUEUE)
        self.viewers.add(q)
        if self._stop:
            self._stop.cancel(); self._stop = None
        if not self.task or self.task.done():
            self.task = self.hass.async_create_background_task(self._upstream(), f"{DOMAIN} camera {self.name}")
        resp = web.StreamResponse(headers={"Content-Type": "video/mp2t", "Cache-Control": "no-store"})
        await resp.prepare(request)
        mux, started = TsMuxer(), False
        try:
            while True:
                item = await q.get()
                if item is None:
                    break
                pts90, au, key = item
                if not started and not key:
                    continue                                # each viewer from a key frame
                started = True
                await resp.write(mux.frame(pts90, au, key, self.config))
        except (ConnectionResetError, asyncio.CancelledError):
            pass
        finally:
            self.viewers.discard(q)
            if not self.viewers and self.task and not self.task.done():
                self._stop = self.hass.loop.call_later(LINGER_S, self._linger_over)
        return resp

    def _linger_over(self) -> None:
        self._stop = None
        if not self.viewers and self.task:
            self.task.cancel()

    def _send(self, item) -> None:
        for q in list(self.viewers):
            try:
                q.put_nowait(item)
            except asyncio.QueueFull:                       # a viewer that does not keep up: let go, it reconnects
                self.viewers.discard(q)
                while not q.empty():
                    q.get_nowait()
                q.put_nowait(None)

    async def _upstream(self) -> None:
        base = None
        try:
            async for t, pts, data in self.c.echo.camera_stream():
                if t == CAM_CONFIG:
                    self.config = data
                    continue
                if base is None:
                    base = pts
                # the Echo's clock is its boot time in us: from 1 s on per stream, far from 2^33's wrap (26.5 h)
                self._send(((pts - base) * 9 // 100 + 90000, data, t == CAM_KEY))
            self.error = ""
        except EchoAuthError:
            self.error = "not logged in"
            self.c.config_entry.async_start_reauth(self.hass)
        except EchoError as e:
            self.error = str(e)
            _LOGGER.warning("%s: camera: %s", self.name, e)
        except asyncio.CancelledError:
            pass
        finally:
            for q in list(self.viewers):
                q.put_nowait(None)
            self.c.async_update_listeners()


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    c: EchoLocalCoordinator = entry.runtime_data
    if not (c.data or {}).get("camera", {}).get("present"):
        return                                              # an Echo without one (every Dot), or an older build
    relay: Relay | None = hass.data.get(f"{DOMAIN}_relay")
    if relay is None:
        relay = hass.data[f"{DOMAIN}_relay"] = Relay()
    await relay.start()
    add([EchoCamera(c, entry, relay)])


class EchoCamera(CoordinatorEntity[EchoLocalCoordinator], Camera):
    _attr_has_entity_name = True
    _attr_translation_key = "camera"
    _attr_supported_features = CameraEntityFeature.STREAM | CameraEntityFeature.ON_OFF

    def __init__(self, c: EchoLocalCoordinator, entry: ConfigEntry, relay: Relay) -> None:
        CoordinatorEntity.__init__(self, c)
        Camera.__init__(self)
        self.relay = relay
        self.feed = Feed(c.hass, c, entry.title)
        self._attr_unique_id = f"{entry.unique_id}_camera"
        self._attr_device_info = device_info(entry, c.echo.hello)

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.relay.feeds[self.feed.token] = self.feed

    async def async_will_remove_from_hass(self) -> None:
        self.relay.feeds.pop(self.feed.token, None)
        if self.feed.task:
            self.feed.task.cancel()
        await super().async_will_remove_from_hass()

    @property
    def _cam(self) -> dict:
        return (self.coordinator.data or {}).get("camera", {})

    @property
    def is_on(self) -> bool:
        """switched on on the Echo, and the lens not covered"""
        return bool(self._cam.get("on")) and self._cam.get("covered") != 1

    @property
    def is_streaming(self) -> bool:
        return bool(self.feed.task and not self.feed.task.done())

    @property
    def extra_state_attributes(self) -> dict:
        a = {"lens_covered": self._cam.get("covered") == 1}
        if self.feed.error:
            a["last_error"] = self.feed.error
        return a

    async def stream_source(self) -> str | None:
        return self.relay.url(self.feed) if self.is_on else None

    async def async_camera_image(self, width: int | None = None, height: int | None = None) -> bytes | None:
        """a still from the stream: joins one that runs, else runs the camera for it (some 3 s, then LINGER_S)"""
        if not self.is_on:
            return None
        return await async_get_image(self.hass, self.relay.url(self.feed), width=width, height=height)

    async def _switch(self, on: bool) -> None:
        await self.coordinator.run(self.coordinator.echo.set(f"camera={'on' if on else 'off'}\n"))

    async def async_turn_on(self) -> None:
        await self._switch(True)

    async def async_turn_off(self) -> None:
        await self._switch(False)
        if self.feed.task:
            self.feed.task.cancel()
