"""The owner's Android apps on an Echo Show (src/hassmic/apps.c on the Echo; docs/screen-apps-launcher.md): the list,
internet per app, an APK added, an app removed, from the sidebar panel's "Apps" tab (frontend/panel.js) and the
action echo_local.install_apk.

The Echo's firewall keeps every app off the internet until the owner allows it per app; installing and removing is
root's (`pm` on the Echo, scripts/device/apps.sh), which reports through GET /api/apps' "install".  An APK reaches the
Echo in pieces of 256 kB over the signed API (api.py apk_send) from a file here: one the browser uploaded to
/api/echo_local/apk/<echo> (kept in a temporary file, up to 200 MB, deleted after), or one under a folder Home
Assistant may read (/config, /media: allowlist_external_dirs).

Websocket (administrators only, as the options are):

    echo_local/apps/list       {echo}                 the Echo's answer, plus "send": this side's upload to the Echo
    echo_local/apps/net        {echo, package, on}    internet for that app
    echo_local/apps/lan        {echo, package, on}    that app reachable from the home network (the ports it listens on:
                                                      a DLNA renderer, a cast receiver; issue #109)
    echo_local/apps/uninstall  {echo, package}        removed (root refuses the screen app)
    echo_local/apps/install    {echo, path}           an APK from a folder Home Assistant may read
    echo_local/apps/fetch      {echo, package}        a known home app's newest release (KNOWN_APPS), downloaded here
                                                      (the Echo has no internet) and sent on as an upload is"""
from __future__ import annotations

import asyncio
import logging
import os
import tempfile
import time

import voluptuous as vol
from aiohttp import web

from homeassistant.components import websocket_api
from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant, ServiceCall, callback
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError, Unauthorized
from homeassistant.helpers import config_validation as cv, device_registry as dr

from .api import EchoError
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)
APK_MAX = 200 * 1024 * 1024             # apps.c's limit
# Home apps made for the Echo (docs/screen-apps.md) that publish a signed APK as a GitHub release asset: package ->
# (repository, asset).  The newest release's is fetched by Home Assistant, which reaches the internet; the Echo's apps
# stay off it until the owner allows them.
KNOWN_APPS = {
    "io.github.nt_f.echowall": ("Nt-f/Echo-Wall", "Echo-Wall.apk"),
}
PKG = vol.All(str, vol.Match(r"^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$"), vol.Length(max=200))


def _sends(hass: HomeAssistant) -> dict:
    return hass.data.setdefault(DOMAIN + "_apk", {})


def _entry(hass: HomeAssistant, echo: str):
    e = next((e for e in hass.config_entries.async_loaded_entries(DOMAIN) if e.unique_id == echo), None)
    if e is None:
        raise HomeAssistantError(f"no Echo {echo} with Echo Local")
    return e


async def install(hass: HomeAssistant, echo: str, path: str, delete: bool = False, name: str | None = None) -> None:
    """send the APK at path to the Echo and have root install it; this side's part in _sends until the Echo's own
    status takes over (its "install" in GET /api/apps)"""
    entry = _entry(hass, echo)
    st = _sends(hass)
    if (st.get(echo) or {}).get("state") == "sending":
        raise HomeAssistantError("an app is being sent to this Echo already")
    size = os.path.getsize(path)
    if size > APK_MAX:
        raise HomeAssistantError("an APK is 200 MB at most")
    st[echo] = {"op": "install", "state": "sending", "sent": 0, "size": size, "error": "", "name": name or os.path.basename(path), "at": time.time()}

    async def run() -> None:
        try:
            echo_ = entry.runtime_data.echo
            n, digest = await echo_.apk_send(path, lambda off: st[echo].update(sent=off))
            await echo_.apk_install(n, digest)
            st[echo].update(state="sent")           # the Echo's own status from here on
        except (EchoError, OSError) as e:
            _LOGGER.warning("%s: the APK was not installed: %s", entry.title, e)
            st[echo].update(state="failed", error=str(e))
        finally:
            if delete:
                try:
                    os.unlink(path)
                except OSError:
                    pass
    entry.async_create_background_task(hass, run(), f"echo_local apk {echo}")


class ApkUploadView(HomeAssistantView):
    """the panel's "Add app": the APK as the request's body, into a temporary file, then sent on in the background"""
    url = "/api/echo_local/apk/{echo}"
    name = "api:echo_local:apk"
    requires_auth = True

    async def post(self, request: web.Request, echo: str) -> web.Response:
        hass: HomeAssistant = request.app["hass"]
        if not request["hass_user"].is_admin:
            raise Unauthorized()
        try:
            _entry(hass, echo)
        except HomeAssistantError as e:
            return self.json_message(str(e), 404)
        if request.content_length is not None and request.content_length > APK_MAX:
            return self.json_message("an APK is 200 MB at most", 413)
        fd, path = tempfile.mkstemp(prefix="echo-local-", suffix=".apk")
        n = 0
        try:
            with os.fdopen(fd, "wb") as f:
                async for chunk in request.content.iter_chunked(1 << 16):
                    n += len(chunk)
                    if n > APK_MAX:
                        raise ValueError("an APK is 200 MB at most")
                    await hass.async_add_executor_job(f.write, chunk)
            if n < 4:
                raise ValueError("empty")
            await install(hass, echo, path, delete=True)
        except (ValueError, HomeAssistantError) as e:
            os.unlink(path)
            return self.json_message(str(e), 400)
        return self.json({"ok": True, "size": n})


async def async_setup(hass: HomeAssistant) -> None:
    hass.http.register_view(ApkUploadView())
    for c in (ws_list, ws_net, ws_lan, ws_uninstall, ws_install, ws_fetch):
        websocket_api.async_register_command(hass, c)

    async def install_apk(call: ServiceCall) -> None:
        path = call.data["path"]
        if not hass.config.is_allowed_path(path) or not os.path.isfile(path):
            raise ServiceValidationError(f"{path}: not a file Home Assistant may read (allowlist_external_dirs)")
        dev = dr.async_get(hass).async_get(call.data["device_id"])
        entries = [e for e in hass.config_entries.async_loaded_entries(DOMAIN) if dev and e.entry_id in dev.config_entries]
        if not entries:
            raise ServiceValidationError("not an Echo of Echo Local")
        await install(hass, entries[0].unique_id, path)
    hass.services.async_register(DOMAIN, "install_apk", install_apk, schema=vol.Schema(
        {vol.Required("device_id"): cv.string, vol.Required("path"): cv.string}))


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/list", vol.Required("echo"): str})
@websocket_api.async_response
async def ws_list(hass: HomeAssistant, connection, msg: dict) -> None:
    try:
        r = await _entry(hass, msg["echo"]).runtime_data.echo.apps()
    except (EchoError, HomeAssistantError) as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    r["send"] = _sends(hass).get(msg["echo"]) or {}
    connection.send_result(msg["id"], r)


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/net", vol.Required("echo"): str,
                                  vol.Required("package"): PKG, vol.Required("on"): bool})
@websocket_api.async_response
async def ws_net(hass: HomeAssistant, connection, msg: dict) -> None:
    try:
        await _entry(hass, msg["echo"]).runtime_data.echo.app_net(msg["package"], msg["on"])
    except (EchoError, HomeAssistantError) as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    connection.send_result(msg["id"])


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/lan", vol.Required("echo"): str,
                                  vol.Required("package"): PKG, vol.Required("on"): bool})
@websocket_api.async_response
async def ws_lan(hass: HomeAssistant, connection, msg: dict) -> None:
    try:
        await _entry(hass, msg["echo"]).runtime_data.echo.app_lan(msg["package"], msg["on"])
    except (EchoError, HomeAssistantError) as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    connection.send_result(msg["id"])


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/uninstall", vol.Required("echo"): str, vol.Required("package"): PKG})
@websocket_api.async_response
async def ws_uninstall(hass: HomeAssistant, connection, msg: dict) -> None:
    try:
        await _entry(hass, msg["echo"]).runtime_data.echo.app_uninstall(msg["package"])
    except (EchoError, HomeAssistantError) as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    # the Echo's status says "ok" for a removal as for an install: which one it was, for the panel
    _sends(hass)[msg["echo"]] = {"op": "uninstall", "state": "sent", "package": msg["package"], "at": time.time()}
    connection.send_result(msg["id"])


async def fetch(hass: HomeAssistant, echo: str, package: str) -> None:
    """a known app's newest release downloaded into a temporary file here, then sent to the Echo and installed; its
    progress in _sends ("downloading", then install()'s)"""
    from homeassistant.helpers.aiohttp_client import async_get_clientsession
    repo, asset = KNOWN_APPS[package]
    entry = _entry(hass, echo)
    st = _sends(hass)
    if (st.get(echo) or {}).get("state") in ("downloading", "sending"):
        raise HomeAssistantError("an app is being sent to this Echo already")
    base = os.environ.get("ECHO_LOCAL_GITHUB", "https://github.com")       # tests: a stand-in GitHub
    url = f"{base}/{repo}/releases/latest/download/{asset}"
    st[echo] = {"op": "install", "state": "downloading", "sent": 0, "size": 0, "error": "", "name": asset, "at": time.time()}

    async def run() -> None:
        fd, path = tempfile.mkstemp(prefix="echo_local_", suffix=".apk")
        try:
            async with async_get_clientsession(hass).get(url, timeout=aiohttp_timeout()) as r:
                if r.status != 200:
                    raise OSError(f"{repo} has no release with {asset} (HTTP {r.status})")
                size, n = r.content_length or 0, 0
                st[echo].update(size=size)
                with os.fdopen(fd, "wb") as f:
                    fd = -1
                    async for piece in r.content.iter_chunked(1 << 16):
                        n += len(piece)
                        if n > APK_MAX:
                            raise OSError("an APK is 200 MB at most")
                        f.write(piece)
                        st[echo].update(sent=n)
            st[echo]["state"] = "downloaded"    # install() takes it from here (and deletes the file)
            await install(hass, echo, path, delete=True, name=asset)
            path = None
        except (OSError, HomeAssistantError, asyncio.TimeoutError) as e:
            _LOGGER.warning("%s: %s not fetched: %s", entry.title, asset, e)
            st[echo].update(state="failed", error=f"not downloaded: {e}")
        except Exception as e:                  # aiohttp's own errors: the network
            _LOGGER.warning("%s: %s not fetched: %s", entry.title, asset, e)
            st[echo].update(state="failed", error=f"not downloaded: {e}")
        finally:
            if fd >= 0:
                os.close(fd)
            if path:
                try:
                    os.unlink(path)
                except OSError:
                    pass
    entry.async_create_background_task(hass, run(), f"echo_local fetch {echo}")


def aiohttp_timeout():
    import aiohttp
    return aiohttp.ClientTimeout(total=600, sock_read=60)


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/fetch", vol.Required("echo"): str, vol.Required("package"): vol.In(list(KNOWN_APPS))})
@websocket_api.async_response
async def ws_fetch(hass: HomeAssistant, connection, msg: dict) -> None:
    try:
        await fetch(hass, msg["echo"], msg["package"])
    except HomeAssistantError as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    connection.send_result(msg["id"])


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/apps/install", vol.Required("echo"): str, vol.Required("path"): str})
@websocket_api.async_response
async def ws_install(hass: HomeAssistant, connection, msg: dict) -> None:
    path = msg["path"]
    if not hass.config.is_allowed_path(path) or not os.path.isfile(path):
        connection.send_error(msg["id"], "invalid_format", f"{path}: not a file Home Assistant may read (allowlist_external_dirs)")
        return
    try:
        await install(hass, msg["echo"], path)
    except (HomeAssistantError, OSError) as e:
        connection.send_error(msg["id"], "echo", str(e))
        return
    connection.send_result(msg["id"])
