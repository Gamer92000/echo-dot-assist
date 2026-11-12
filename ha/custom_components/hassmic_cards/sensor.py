"""Each Echo Show's "Card on screen": the card it shows (its id where an automation named it, else its title; "none"),
whether a voice request's or a notice, and the last card it refused (why, when; cleared when a card shows again), as the
Echo reports them (esphome.hassmic_card_state / _refused)."""
from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.restore_state import RestoreEntity
from homeassistant.util import dt as dt_util

from .entity import ShowEntity, setup_shows


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    setup_shows(hass, entry, add, lambda node, dev: [CardOnScreen(hass, node, dev)])


class CardOnScreen(ShowEntity, SensorEntity, RestoreEntity):
    _attr_icon = "mdi:card-text-outline"

    def __init__(self, hass: HomeAssistant, node: str, device_id: str) -> None:
        super().__init__(hass, node, device_id, "card_on_screen")
        self._attr_native_value = "none"
        self._attr_extra_state_attributes = {"card": "", "title": "", "scope": "", "since": None, "refused": None, "refused_at": None}

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        if (last := await self.async_get_last_state()) is not None:   # (until the Echo tells again: on its next link)
            self._attr_native_value = last.state if last.state not in ("unknown", "unavailable") else "none"
            self._attr_extra_state_attributes.update({k: last.attributes.get(k) for k in self._attr_extra_state_attributes})

    @callback
    def _on_echo(self, kind: str, data: dict) -> None:
        a = self._attr_extra_state_attributes
        if kind == "state":
            shown = data.get("shown")
            cid = data.get("card", "") if data.get("named") else ""   # (an id hassmic cards made up: its title instead)
            self._attr_native_value = (cid or data.get("title") or "card") if shown else "none"
            a.update(card=data.get("card", "") if shown else "", title=data.get("title", "") if shown else "",
                     scope=data.get("scope", "") if shown else "", since=dt_util.utcnow().isoformat())
            if shown:
                a.update(refused=None, refused_at=None)         # (a card took: the last refusal is past)
        elif kind == "refused":
            a.update(refused=data.get("refused"), refused_at=dt_util.utcnow().isoformat())
        else:
            return
        self.async_write_ha_state()
