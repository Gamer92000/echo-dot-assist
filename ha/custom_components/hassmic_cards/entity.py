"""What hassmic cards adds to each Echo Show's device (its own entities, on the Echo's device page)."""
from __future__ import annotations

from collections.abc import Callable

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import area_registry as ar
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import Entity
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN, SIGNAL_DEVICE, SIGNAL_NEW_SHOW


class ShowEntity(Entity):
    """an entity of one Echo Show.  Home Assistant keeps a device to one integration, so hassmic cards has a device of its
    own per Echo Show: the Echo's name and MAC, in its area (its entities read "Echo Show 8 1a2b3c Card button")"""
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, hass: HomeAssistant, node: str, device_id: str, key: str) -> None:
        self.node, self.device_id = node, device_id
        self._attr_unique_id = f"{node}_{key}"
        self._attr_translation_key = key
        dev = dr.async_get(hass).async_get(device_id)
        name = (dev.name_by_user or dev.name) if dev else node
        area = ar.async_get(hass).async_get_area(dev.area_id) if dev and dev.area_id else None
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, node)}, connections=set(dev.connections) if dev else set(),
                                            name=name, manufacturer="hassmic", model="Echo Show cards",
                                            suggested_area=area.name if area else None)

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(async_dispatcher_connect(self.hass, SIGNAL_DEVICE.format(self.device_id), self._on_echo))
        # available while the Echo is: as its ESPHome entities are (the assist satellite's, else any of the device's)
        ents = [e for e in er.async_entries_for_device(er.async_get(self.hass), self.device_id) if e.platform == "esphome"]
        watch = next((e.entity_id for e in ents if e.domain == "assist_satellite"), None) or next((e.entity_id for e in ents), None)
        if watch:
            def follow(*_: object) -> None:
                st = self.hass.states.get(watch)
                avail = st is not None and st.state != "unavailable"
                if avail != self._attr_available:
                    self._attr_available = avail
                    if self.hass is not None and self.entity_id:
                        self.async_write_ha_state()
            st = self.hass.states.get(watch)
            self._attr_available = st is not None and st.state != "unavailable"
            self.async_on_remove(async_track_state_change_event(self.hass, [watch], callback(follow)))

    @callback
    def _on_echo(self, kind: str, data: dict) -> None:
        """an event of this Echo's (kind: action, state, refused)"""


def setup_shows(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback,
                make: Callable[[str, str], list[Entity]]) -> None:
    """entities for every Echo Show there is, and for one adopted later"""
    seen: set[str] = set()

    @callback
    def update() -> None:
        new = []
        for node, device_id in entry.runtime_data.shows():
            if node not in seen:
                seen.add(node)
                new += make(node, device_id)
        if new:
            add(new)
    update()
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_NEW_SHOW, update))
