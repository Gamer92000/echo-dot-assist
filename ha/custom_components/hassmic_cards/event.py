"""Each Echo Show's "Card button": an event when a button on one of its cards is pressed (attributes: action, card),
for automations made in the UI (an event entity's trigger)."""
from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .entity import ShowEntity, setup_shows


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    setup_shows(hass, entry, add, lambda node, dev: [CardButton(hass, node, dev)])


class CardButton(ShowEntity, EventEntity):
    _attr_event_types = ["pressed"]
    _attr_icon = "mdi:gesture-tap-button"

    def __init__(self, hass: HomeAssistant, node: str, device_id: str) -> None:
        super().__init__(hass, node, device_id, "card_button")

    @callback
    def _on_echo(self, kind: str, data: dict) -> None:
        if kind == "action":
            self._trigger_event("pressed", {"action": data.get("action", ""), "card": data.get("card", "")})
            self.async_write_ha_state()
