"""A switch per alarm: on, it rings; off, it is kept.  Its attributes are the alarm (time, days, label, when next), for
the dashboard card and templates; changed with the entity actions echo_local.edit_alarm and echo_local.delete_alarm."""
from __future__ import annotations

from datetime import datetime, timezone

import voluptuous as vol

from homeassistant.components.switch import SwitchEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import config_validation as cv, device_registry as dr, entity_platform, entity_registry as er
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import EchoLocalCoordinator, days_mask
from .const import CONF_MAC, CONF_NODE, DAY_KEYS, DOMAIN


def device_info(entry: ConfigEntry, hello: dict) -> DeviceInfo:
    info = DeviceInfo(identifiers={(DOMAIN, entry.data[CONF_NODE])}, name=entry.title, manufacturer="hassmic",
                      model=hello.get("model"), sw_version=hello.get("version"))
    if entry.data.get(CONF_MAC):        # the esphome integration's device has the same (Home Assistant 2026 keeps the two
                                        # integrations' devices apart; the card finds the other by it)
        info["connections"] = {(dr.CONNECTION_NETWORK_MAC, dr.format_mac(entry.data[CONF_MAC]))}
    return info


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    c: EchoLocalCoordinator = entry.runtime_data
    have: dict[int, AlarmSwitch] = {}

    @callback
    def sync() -> None:
        """new alarms get a switch; deleted ones lose theirs (and their entity registry entry: ids are never reused
        while one is kept, so a new alarm is a new entity)"""
        now = {a["id"] for a in (c.data or {}).get("alarms", [])}
        new = [AlarmSwitch(c, entry, i) for i in now - have.keys()]
        for e in new:
            have[e.alarm_id] = e
        if new:
            add(new)
        reg = er.async_get(hass)
        for i in list(have.keys() - now):
            e = have.pop(i)
            if e.entity_id and reg.async_get(e.entity_id):
                reg.async_remove(e.entity_id)

    sync()
    entry.async_on_unload(c.async_add_listener(sync))
    # Settings HA has only from here (the Echo's ESPHome entities leave them to its page): listed where the Echo has them
    if "own_voice_replies" in (c.data or {}).get("settings", {}):
        add([SettingSwitch(c, entry, "own_voice_replies", "Replies in the Echo's own voice", "mdi:account-voice")])

    platform = entity_platform.async_get_current_platform()
    platform.async_register_entity_service("edit_alarm", {
        vol.Optional("time"): cv.time, vol.Optional("days"): vol.Any(cv.string, [cv.string]),
        vol.Optional("label"): cv.string, vol.Optional("enabled"): cv.boolean}, "async_edit")
    platform.async_register_entity_service("delete_alarm", {}, "async_delete")


class AlarmSwitch(CoordinatorEntity[EchoLocalCoordinator], SwitchEntity):
    _attr_has_entity_name = True
    _attr_icon = "mdi:alarm"

    def __init__(self, c: EchoLocalCoordinator, entry: ConfigEntry, alarm_id: int) -> None:
        super().__init__(c)
        self.alarm_id = alarm_id
        self._attr_unique_id = f"{entry.data[CONF_NODE]}-alarm-{alarm_id}"
        self._attr_device_info = device_info(entry, c.echo.hello)

    @property
    def alarm(self) -> dict | None:
        return next((a for a in (self.coordinator.data or {}).get("alarms", []) if a["id"] == self.alarm_id), None)

    @property
    def available(self) -> bool:
        return super().available and self.alarm is not None

    @property
    def name(self) -> str:
        a = self.alarm
        if not a:
            return f"Alarm {self.alarm_id}"
        return f"Alarm {a['label']}" if a["label"] else f"Alarm {a['text']}"

    @property
    def is_on(self) -> bool:
        a = self.alarm
        return bool(a and a["enabled"])

    @property
    def extra_state_attributes(self) -> dict:
        a = self.alarm
        if not a:
            return {}
        ts = lambda t: datetime.fromtimestamp(t, timezone.utc).isoformat() if t else None
        return {"alarm_id": a["id"], "hour": a["hour"], "minute": a["min"], "time": f"{a['hour']:02d}:{a['min']:02d}",
                "days": [DAY_KEYS[d] for d in (1, 2, 3, 4, 5, 6, 0) if a["days"] & 1 << d], "repeats": a["days"] != 0,
                "label": a["label"], "description": a["text"], "next_ring": ts(a["next"]), "snoozed_until": ts(a["snoozed"]),
                "h24": (self.coordinator.data or {}).get("h24", True), "ringing": (self.coordinator.data or {}).get("ringing", False)}

    async def _put(self, **change) -> None:
        a = dict(self.alarm or {})
        if not a:
            return
        a.update(change)
        await self.coordinator.run(self.coordinator.echo.put_alarm(a["id"], a["hour"], a["min"], a["days"], a["enabled"], a["label"]))

    async def async_turn_on(self, **kwargs) -> None:
        await self._put(enabled=True)

    async def async_turn_off(self, **kwargs) -> None:
        await self._put(enabled=False)

    async def async_edit(self, time=None, days=None, label=None, enabled=None) -> None:
        change = {}
        if time is not None:
            change.update(hour=time.hour, min=time.minute)
        if days is not None:
            change["days"] = days_mask(days)
        if label is not None:
            change["label"] = label
        change["enabled"] = True if enabled is None else enabled     # an alarm edited is one meant to ring
        await self._put(**change)

    async def async_delete(self) -> None:
        await self.coordinator.run(self.coordinator.echo.delete_alarm(self.alarm_id))


class SettingSwitch(CoordinatorEntity[EchoLocalCoordinator], SwitchEntity):
    """One of the Echo's on/off settings (settings.c), changed through its signed /api/set as the settings page does.
    own_voice_replies: Home Assistant's replies said on the Echo in its own voice from their text, streamed ones
    sentence by sentence as the agent writes them, and Home Assistant's audio of them never fetched (docs/GUIDE.md
    "The Echo's own voice")."""
    _attr_has_entity_name = True

    def __init__(self, c: EchoLocalCoordinator, entry: ConfigEntry, key: str, name: str, icon: str) -> None:
        super().__init__(c)
        self.key = key
        self._attr_name = name
        self._attr_icon = icon
        self._attr_unique_id = f"{entry.data[CONF_NODE]}-setting-{key}"
        self._attr_device_info = device_info(entry, c.echo.hello)

    @property
    def available(self) -> bool:
        return super().available and self.key in (self.coordinator.data or {}).get("settings", {})

    @property
    def is_on(self) -> bool:
        return (self.coordinator.data or {}).get("settings", {}).get(self.key) in ("1", "on", "true")

    async def async_turn_on(self, **kwargs) -> None:
        await self.coordinator.run(self.coordinator.echo.set(f"{self.key}=on\n"))

    async def async_turn_off(self, **kwargs) -> None:
        await self.coordinator.run(self.coordinator.echo.set(f"{self.key}=off\n"))
