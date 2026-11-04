"""Echo Local, the Home Assistant integration for Echos running hassmic: the Echo's own alarms (src/hassmic/alarms.c) as entities, actions and a dashboard card.

The esphome integration already has the Echo (voice, media player, "Next alarm", "Snooze alarm", ...).  This one adds
what ESPHome's entities cannot carry: every alarm with its time, days, label and switch, and adding, changing and
deleting them, over the Echo's settings page API (api.py).  Its entities land on the same device: Home Assistant keys
devices on the Wi-Fi MAC, which both report."""
from __future__ import annotations

import logging
from datetime import timedelta
from pathlib import Path

import voluptuous as vol

from homeassistant.components.http import StaticPathConfig
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, Platform
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import ConfigEntryAuthFailed, ConfigEntryNotReady, HomeAssistantError, ServiceValidationError
from homeassistant.helpers import config_validation as cv, device_registry as dr
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.typing import ConfigType
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import DEFAULT_PORT, Echo, EchoAuthError, EchoError
from . import apps, kiosk, panel
from .const import CARD_URL, CONF_KEY, DAY_KEYS, DOMAIN, PANEL_PATH, PANEL_URL, SCAN_INTERVAL_S

_LOGGER = logging.getLogger(__name__)
PLATFORMS = [Platform.SWITCH]
CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)


def days_mask(days) -> int:
    """["mon", "fri"], "weekdays", "every day", "" (once) -> alarms.h's bits"""
    if days in (None, "", "once", []):
        return 0
    if isinstance(days, str):
        words = {"daily": 0x7F, "every day": 0x7F, "everyday": 0x7F, "weekdays": 0x3E, "weekends": 0x41}
        if days.strip().lower() in words:
            return words[days.strip().lower()]
        days = [d for d in days.replace(",", " ").split()]
    mask = 0
    for d in days:
        k = str(d).strip().lower()[:3]
        if k not in DAY_KEYS:
            raise ServiceValidationError(f"not a day: {d}")
        mask |= 1 << DAY_KEYS.index(k)
    return mask


class EchoLocalCoordinator(DataUpdateCoordinator[dict]):
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, echo: Echo) -> None:
        super().__init__(hass, _LOGGER, name=entry.title, update_interval=timedelta(seconds=SCAN_INTERVAL_S), config_entry=entry)
        self.echo = echo

    async def _async_update_data(self) -> dict:
        try:
            return await self.echo.alarms()
        except EchoAuthError as e:
            raise ConfigEntryAuthFailed(str(e)) from e
        except EchoError as e:
            raise UpdateFailed(str(e)) from e

    async def run(self, what) -> None:
        """a change on the Echo, then the list read again at once (the next poll would be up to 30 s on)"""
        try:
            await what
        except EchoAuthError as e:
            self.config_entry.async_start_reauth(self.hass)
            raise HomeAssistantError(f"{self.config_entry.title}: Home Assistant is not let in any more") from e
        except EchoError as e:
            raise HomeAssistantError(f"{self.config_entry.title}: {e}") from e
        await self.async_refresh()          # at once: async_request_refresh waits out its 10 s cooldown


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    # the dashboard card, served from here and loaded on every dashboard
    await panel.async_setup(hass)         # the sidebar panel's commands: the Echos, their screen's setup
    await apps.async_setup(hass)          # the owner's Android apps on an Echo Show: list, internet, add, remove
    fe = Path(__file__).parent / "frontend"
    # the card and the panel: no login to fetch them (only code), the panel's data then over the websocket with its login
    await hass.http.async_register_static_paths([StaticPathConfig(CARD_URL, str(fe / "echo-local-alarms-card.js"), False),
                                                 StaticPathConfig(PANEL_URL, str(fe / "panel.js"), False)])
    # the sidebar's "Echo Local": every Echo Show's screen (which pages, which app) and its Android apps (frontend/panel.js);
    # administrators only, as the options it changes are.  A home screen app arranges its Home itself (Echo-Wall: its
    # own integration)
    from homeassistant.components import panel_custom
    await panel_custom.async_register_panel(hass, frontend_url_path=PANEL_PATH, webcomponent_name="echo-local-panel",
                                            sidebar_title="Echo Local", sidebar_icon="mdi:tablet-dashboard",
                                            module_url=PANEL_URL, require_admin=True, embed_iframe=False)
    from homeassistant.components.frontend import add_extra_js_url
    add_extra_js_url(hass, CARD_URL)
    add_extra_js_url(hass, CARD_URL, es5=True)      # the legacy build too (older browsers); defined once either way

    def coordinator_of(call: ServiceCall) -> EchoLocalCoordinator:
        """the Echo a call names by its device; the only one when there is one"""
        entries = [e for e in hass.config_entries.async_loaded_entries(DOMAIN)]
        dev = call.data.get("device_id")
        if dev:
            d = dr.async_get(hass).async_get(dev)
            entries = [e for e in entries if d and e.entry_id in d.config_entries]
        if len(entries) != 1:
            raise ServiceValidationError("name the Echo (device_id)" if entries else "no such Echo with hassmic")
        return entries[0].runtime_data

    async def add_alarm(call: ServiceCall) -> None:
        c = coordinator_of(call); t = call.data["time"]
        await c.run(c.echo.put_alarm(0, t.hour, t.minute, days_mask(call.data.get("days")), call.data.get("enabled", True), call.data.get("label", "")))

    async def snooze(call: ServiceCall) -> None:
        c = coordinator_of(call); await c.run(c.echo.snooze())

    async def stop(call: ServiceCall) -> None:
        c = coordinator_of(call); await c.run(c.echo.stop())

    dev = {vol.Optional("device_id"): cv.string}
    hass.services.async_register(DOMAIN, "add_alarm", add_alarm, schema=vol.Schema({
        **dev, vol.Required("time"): cv.time, vol.Optional("days", default=[]): vol.Any(cv.string, [cv.string]),
        vol.Optional("label", default=""): cv.string, vol.Optional("enabled", default=True): cv.boolean}))
    hass.services.async_register(DOMAIN, "snooze", snooze, schema=vol.Schema(dev))
    hass.services.async_register(DOMAIN, "stop", stop, schema=vol.Schema(dev))
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    echo = Echo(async_get_clientsession(hass), entry.data[CONF_HOST], entry.data.get("port", DEFAULT_PORT), entry.data[CONF_KEY])
    try:
        hello = await echo.get_hello()
    except EchoError as e:
        raise ConfigEntryNotReady(str(e)) from e
    if hello.get("node") != entry.unique_id:
        raise ConfigEntryNotReady(f"{entry.data[CONF_HOST]} is {hello.get('name')} now, not {entry.title}")
    coordinator = EchoLocalCoordinator(hass, entry, echo)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    if kiosk.OPT_KIOSK not in entry.options:
        # never set up from this entry: an Echo that shows a screen already (a replaced entry's) keeps it, with this
        # entry's own login (the old one went with the old entry's user)
        try:
            new = kiosk.adopt(hass, await echo.kiosk())
        except EchoError:
            new = None
        if new:
            _LOGGER.info("%s: taking over the screen the Echo shows (%s)", entry.title, ", ".join(new[kiosk.OPT_SCREENS]))
            hass.config_entries.async_update_entry(entry, options={**entry.options, **new})
    if entry.options.get(kiosk.OPT_KIOSK) or entry.data.get(kiosk.DATA_USER):
        try:
            await kiosk.push(hass, entry, echo)
        except EchoError:
            pass                            # logged; the next start or options change sends it again
    return True


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await kiosk.remove(hass, entry)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
