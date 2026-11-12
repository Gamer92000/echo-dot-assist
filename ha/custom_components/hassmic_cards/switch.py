"""Each Echo Show's "Voice cards" switch: cards for voice requests on that Show (on by default; the integration's option
switches them off everywhere). Automation cards are not affected."""
from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.restore_state import RestoreEntity

from .entity import ShowEntity, setup_shows


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    setup_shows(hass, entry, add, lambda node, dev: [VoiceCards(hass, entry, node, dev)])


class VoiceCards(ShowEntity, SwitchEntity, RestoreEntity):
    _attr_entity_category = EntityCategory.CONFIG
    _attr_icon = "mdi:account-voice"

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, node: str, device_id: str) -> None:
        super().__init__(hass, node, device_id, "voice_cards")
        self._cards = entry.runtime_data
        self._attr_is_on = True

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        if (last := await self.async_get_last_state()) is not None and last.state == "off":
            self._attr_is_on = False
        self._apply()

    def _apply(self) -> None:
        (self._cards.voice_off.discard if self._attr_is_on else self._cards.voice_off.add)(self.node)

    async def async_turn_on(self, **kwargs: Any) -> None:
        self._attr_is_on = True
        self._apply()
        self.async_write_ha_state()

    async def async_turn_off(self, **kwargs: Any) -> None:
        self._attr_is_on = False
        self._apply()
        self.async_write_ha_state()
