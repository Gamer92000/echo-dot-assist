"""The Echo Show's home app (src/hassmic/kiosk.c, docs/screen-apps.md): which app it is, and what it shows, set up
from here.

The integration's options (config_flow.py OptionsFlow, or the sidebar panel's Screen tab) pick the home app (any app
installed on the Echo: the same choice as the settings page's Screen page, the last one made counts) and its screens:
Home (hassmic:home, drawn by the app: Echo-Wall), the clock, a dashboard of Home Assistant's own UI, a page of one's
own; the drawer offers them by name.  Music is not among them: the drawer's Music is the Echo's own Music Assistant
player (src/ui/music.c, set up on the Echo's settings page); Music Assistant's web page as a screen went with it.
The Echo has no keyboard, so it gets a login of its own: a Home Assistant user
made for this Echo ("<Echo> kiosk", not an administrator unless the options say so) and a long-lived token of it, sealed
for the Echo on the way.  Removing the integration's entry removes that user."""
from __future__ import annotations

import logging
import time
from datetime import timedelta

from homeassistant.auth.const import GROUP_ID_ADMIN, GROUP_ID_USER
from homeassistant.auth.models import TOKEN_TYPE_LONG_LIVED_ACCESS_TOKEN
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.network import NoURLAvailableError, get_url

from .api import Echo, EchoError

_LOGGER = logging.getLogger(__name__)

OPT_KIOSK = "kiosk"                     # on: the home app gets the screens and a login; off: neither
OPT_SCREENS = "kiosk_screens"           # of SCREEN_KINDS, in this order
OPT_DASHBOARD = "kiosk_dashboard"       # url path of the dashboard ("lovelace" = Overview)
OPT_MUSIC = "kiosk_music"               # gone (the drawer's own Music): read nowhere, left in old entries' options
OPT_CUSTOM_NAME = "kiosk_custom_name"
OPT_CUSTOM_URL = "kiosk_custom_url"
OPT_DEFAULT = "kiosk_default"           # the kind it starts on
OPT_RETURN = "kiosk_return"             # seconds untouched before going back to it (0 never)
OPT_ADMIN = "kiosk_admin"               # the kiosk's user an administrator (Home Assistant's settings on the Echo)
OPT_URL = "kiosk_ha_url"                # Home Assistant's address as the Echo reaches it; empty: get_url's
OPT_APP = "kiosk_app"                   # the home app's package (docs/screen-apps.md); empty: the settings page's choice
OPT_APP_SETTINGS = "kiosk_app_settings"  # "key=value" lines for the screen app (an Immich address and key, ...)


def app_settings(text: str) -> dict[str, str]:
    """the options' "key=value" lines; blank lines and # comments left out"""
    out: dict[str, str] = {}
    for line in (text or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip().lower()] = v.strip()
    return out

SCREEN_KINDS = ["wall", "dashboard", "clock", "custom"]
DATA_USER, DATA_TOKEN = "kiosk_user", "kiosk_token"




def ha_url(hass: HomeAssistant, entry: ConfigEntry) -> str:
    u = (entry.options.get(OPT_URL) or "").strip().rstrip("/")
    if u:
        return u
    try:
        # the address the LAN reaches: the Echo's egress lock lets it reach local addresses only
        return get_url(hass, allow_external=False, allow_cloud=False, prefer_external=False).rstrip("/")
    except NoURLAvailableError:
        return get_url(hass).rstrip("/")


def screens(hass: HomeAssistant, entry: ConfigEntry) -> tuple[list[tuple[str, str]], int]:
    o, base = entry.options, ha_url(hass, entry)
    kinds = [k for k in SCREEN_KINDS if k in (o.get(OPT_SCREENS) or ["wall", "dashboard"])]
    out: list[tuple[str, str, str]] = []
    for k in kinds:
        if k == "clock":                # drawn by a screen app that can (docs/screen-apps.md); hassmic's own kiosk skips it
            out.append((k, "Clock", "hassmic:clock"))
        elif k == "wall":               # the screen app's own Home (Echo-Wall: its layout is its own integration's)
            out.append((k, "Home", "hassmic:home"))
        elif k == "dashboard":
            out.append((k, "Home Assistant", f"{base}/{(o.get(OPT_DASHBOARD) or 'lovelace').strip('/')}"))
        elif k == "custom":
            u = (o.get(OPT_CUSTOM_URL) or "").strip()
            if u.startswith(("http://", "https://")):
                out.append((k, (o.get(OPT_CUSTOM_NAME) or "Page").strip()[:40], u))
    want = o.get(OPT_DEFAULT) or (kinds[0] if kinds else "wall")
    default = next((i for i, s in enumerate(out) if s[0] == want), 0)
    return [(n, u) for _, n, u in out], default


async def login(hass: HomeAssistant, entry: ConfigEntry) -> str:
    """the kiosk's user and token: made once, kept in the entry's data; a new token if the old one is gone"""
    admin = bool(entry.options.get(OPT_ADMIN))
    groups = [GROUP_ID_ADMIN if admin else GROUP_ID_USER]
    user = None
    if uid := entry.data.get(DATA_USER):
        user = await hass.auth.async_get_user(uid)
    if user is None:
        user = await hass.auth.async_create_user(f"{entry.title} kiosk", group_ids=groups, local_only=True)
        hass.config_entries.async_update_entry(entry, data={**entry.data, DATA_USER: user.id, DATA_TOKEN: ""})
    elif [g.id for g in user.groups] != groups:
        await hass.auth.async_update_user(user, group_ids=groups)
    token = entry.data.get(DATA_TOKEN) or ""
    if token and hass.auth.async_validate_access_token(token) is not None:
        return token
    for rt in list(user.refresh_tokens.values()):        # a token of an earlier setup: one at a time
        hass.auth.async_remove_refresh_token(rt)
    rt = await hass.auth.async_create_refresh_token(user, client_name=f"Echo Local kiosk ({entry.unique_id})",
                                                    token_type=TOKEN_TYPE_LONG_LIVED_ACCESS_TOKEN,
                                                    access_token_expiration=timedelta(days=3650))
    token = hass.auth.async_create_access_token(rt)
    hass.config_entries.async_update_entry(entry, data={**entry.data, DATA_TOKEN: token})
    return token


def last_push(hass: HomeAssistant, entry: ConfigEntry) -> dict:
    """what came of the last push to this Echo since Home Assistant started: {at, ok, error, screens} ({} none yet)"""
    return dict(hass.data.get(DOMAIN_PUSH, {}).get(entry.entry_id) or {})


DOMAIN_PUSH = "echo_local_kiosk_push"


async def push(hass: HomeAssistant, entry: ConfigEntry, echo: Echo) -> None:
    """the options onto the Echo: its setting, then the screens; the outcome kept for the panel (a push that never
    lands leaves the Echo with a login Home Assistant refuses, and the logs are the only other place that says so)"""
    o = entry.options
    done = hass.data.setdefault(DOMAIN_PUSH, {})
    try:
        if not o.get(OPT_KIOSK):                       # no screens and no login for the home app (it stays the home app)
            await echo.put_kiosk(ha_url(hass, entry), [], 0, int(o.get(OPT_RETURN, 120)), None, "", {})
            done[entry.entry_id] = {"at": time.time(), "ok": True, "error": "", "screens": 0, "kiosk": False}
            return
        scr, default = screens(hass, entry)
        if not scr:
            _LOGGER.warning("%s: the kiosk has no screen to show", entry.title)
        token = await login(hass, entry)
        await echo.put_kiosk(ha_url(hass, entry), scr, default, int(o.get(OPT_RETURN, 120)), token,
                             (o.get(OPT_APP) or "").strip(), app_settings(o.get(OPT_APP_SETTINGS, "")))
        _LOGGER.info("%s: the screen's setup sent (%d screens, home app %s)", entry.title, len(scr), o.get(OPT_APP) or "as the Echo has it")
        done[entry.entry_id] = {"at": time.time(), "ok": True, "error": "", "screens": len(scr), "kiosk": True}
    except EchoError as e:
        _LOGGER.warning("%s: the kiosk could not be set up: %s", entry.title, e)
        done[entry.entry_id] = {"at": time.time(), "ok": False, "error": str(e)}
        raise


def adopt(hass: HomeAssistant, echo_cfg: dict) -> dict | None:
    """options from the screen the Echo shows now (GET /api/kiosk), for an entry that never set one: a replaced
    entry (the Echo keeps the old one's screens, with a login the removed entry's user took along) takes them over and
    sends them again with a login of its own.  None when the Echo shows nothing"""
    if not echo_cfg.get("enabled") or not echo_cfg.get("screens"):
        return None
    kinds, o = [], {}
    for s in echo_cfg["screens"]:
        u, name = s.get("url", ""), s.get("name", "")
        if u == "hassmic:home" or "/wall.html" in u:
            k = "wall"
        elif u == "hassmic:clock":
            k = "clock"
        elif "music_assistant" in u or name == "Music":
            continue                    # the drawer's own Music now
        elif u.startswith(echo_cfg.get("ha", "") + "/") and name == "Home Assistant":
            k = "dashboard"; o[OPT_DASHBOARD] = u[len(echo_cfg["ha"]) + 1:] or "lovelace"
        elif u.startswith(("http://", "https://")):
            k = "custom"; o[OPT_CUSTOM_NAME] = name[:40]; o[OPT_CUSTOM_URL] = u
        else:
            continue
        if k not in kinds:
            kinds.append(k)
    if not kinds:
        return None
    d = echo_cfg.get("default", 0)
    o.update({OPT_KIOSK: True, OPT_SCREENS: kinds, OPT_DEFAULT: kinds[d] if 0 <= d < len(kinds) else kinds[0],
              OPT_RETURN: int(echo_cfg.get("return", 120)), OPT_APP: echo_cfg.get("app", "")})
    return o


async def remove(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """the entry is gone: so is its kiosk's user (and with it the token the Echo holds)"""
    if uid := entry.data.get(DATA_USER):
        if user := await hass.auth.async_get_user(uid):
            await hass.auth.async_remove_user(user)
