"""The sidebar panel's commands (frontend/panel.js): the Echos with a screen, their screen's setup (kiosk.py), which
the options flow also sets.  The Echo Show's own apps have theirs in apps.py.  Administrators only, as the options are.

    echo_local/list                                admin: the Echos (node, title, whether the screen is on, its app)
    echo_local/kiosk/get   {echo}                  admin: the screen's options, for the panel
    echo_local/kiosk/set   {echo, options}         admin: the same, saved; the entry reloads and sends them on
    echo_local/kiosk/push  {echo}                  admin: sent to the Echo again now

The home screen an app draws (Echo-Wall's) is that app's own business: Echo-Wall has an integration of its own
(Nt-f/Echo-Wall, custom_components/echo_wall); Echo Local only sets which app shows the screen."""
from __future__ import annotations

import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.core import HomeAssistant, callback

from .const import DOMAIN


def _entries(hass: HomeAssistant):
    return hass.config_entries.async_entries(DOMAIN)


async def async_setup(hass: HomeAssistant) -> None:
    for c in (ws_list, ws_kiosk_get, ws_kiosk_set, ws_kiosk_push):
        websocket_api.async_register_command(hass, c)


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/list"})
@callback
def ws_list(hass: HomeAssistant, connection, msg: dict) -> None:
    from . import kiosk
    connection.send_result(msg["id"], {"echos": [
        {"echo": e.unique_id or "", "title": e.title, "entry_id": e.entry_id, "kiosk": bool(e.options.get(kiosk.OPT_KIOSK)),
         "app": (e.options.get(kiosk.OPT_APP) or "").strip(), "screens": e.options.get(kiosk.OPT_SCREENS) or ["wall", "dashboard"],
         # the Home Assistant user made for this Echo (kiosk.py): the one Echo-Wall's app logs in as, which its "Home"
         # tab (Echo-Wall's own panel, embedded) shows the device of
         "user": e.data.get(kiosk.DATA_USER) or ""}
        for e in _entries(hass)]})


# ---------------------------------------------------------------- the screen's options (kiosk.py), for the panel

def _kiosk_schema():
    from . import kiosk as k
    return vol.Schema({
        vol.Optional(k.OPT_KIOSK): bool,
        vol.Optional(k.OPT_SCREENS): vol.All([vol.In(k.SCREEN_KINDS)], vol.Length(max=len(k.SCREEN_KINDS))),
        vol.Optional(k.OPT_DEFAULT): vol.In(k.SCREEN_KINDS),
        vol.Optional(k.OPT_DASHBOARD): vol.All(str, vol.Length(max=200)),
        vol.Optional(k.OPT_CUSTOM_NAME): vol.All(str, vol.Length(max=40)),
        vol.Optional(k.OPT_CUSTOM_URL): vol.All(str, vol.Length(max=400)),
        vol.Optional(k.OPT_RETURN): vol.All(vol.Coerce(int), vol.Range(min=0, max=3600)),
        vol.Optional(k.OPT_ADMIN): bool,
        vol.Optional(k.OPT_URL): vol.All(str, vol.Length(max=400)),
        vol.Optional(k.OPT_APP): vol.All(str, vol.Match(r"^([a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+)?$"), vol.Length(max=120)),
        vol.Optional(k.OPT_APP_SETTINGS): vol.All(str, vol.Length(max=8000)),
    })


def _entry(hass, echo):
    return next((e for e in _entries(hass) if e.unique_id == echo), None)


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/kiosk/get", vol.Required("echo"): str})
@callback
def ws_kiosk_get(hass: HomeAssistant, connection, msg: dict) -> None:
    from . import kiosk as k
    e = _entry(hass, msg["echo"])
    if e is None:
        connection.send_error(msg["id"], "not_found", "no such Echo")
        return
    connection.send_result(msg["id"], {"options": dict(e.options), "kinds": k.SCREEN_KINDS,
                                       "ha_url": k.ha_url(hass, e), "push": k.last_push(hass, e)})


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/kiosk/push", vol.Required("echo"): str})
@websocket_api.async_response
async def ws_kiosk_push(hass: HomeAssistant, connection, msg: dict) -> None:
    """the screen's setup sent to the Echo again now (a fresh login with it, where the kiosk is on)"""
    from . import kiosk as k
    from .api import EchoError
    e = _entry(hass, msg["echo"])
    if e is None or not hasattr(e, "runtime_data"):
        connection.send_error(msg["id"], "not_found", "no such Echo (or not loaded)")
        return
    try:
        await k.push(hass, e, e.runtime_data.echo)
    except EchoError as err:
        connection.send_error(msg["id"], "echo", str(err))
        return
    connection.send_result(msg["id"], {"push": k.last_push(hass, e)})


@websocket_api.require_admin
@websocket_api.websocket_command({vol.Required("type"): "echo_local/kiosk/set", vol.Required("echo"): str, vol.Required("options"): dict})
@websocket_api.async_response
async def ws_kiosk_set(hass: HomeAssistant, connection, msg: dict) -> None:
    e = _entry(hass, msg["echo"])
    if e is None:
        connection.send_error(msg["id"], "not_found", "no such Echo")
        return
    try:
        new = _kiosk_schema()(msg["options"])
    except vol.Invalid as err:
        connection.send_error(msg["id"], "invalid_format", str(err))
        return
    hass.config_entries.async_update_entry(e, options={**e.options, **new})
    # as the options flow's Submit (OptionsFlowWithReload): the setup sends it all to the Echo
    hass.config_entries.async_schedule_reload(e.entry_id)
    connection.send_result(msg["id"], {"options": dict(e.options)})
