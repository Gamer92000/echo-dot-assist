"""Notify entities: one per Echo Show ("Screen") and one for every Show ("All Echo Show screens"): notify.send_message
puts a message card on the screen (its title and words, 30 s). Pictures, buttons, cards that stay: hassmic_cards.show_message
or hassmic_cards.show."""
from __future__ import annotations

from homeassistant.components.notify import NotifyEntity, NotifyEntityFeature
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import ShowEntity, setup_shows

TIMEOUT = 30


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, add: AddEntitiesCallback) -> None:
    add([AllShows(entry)])
    setup_shows(hass, entry, add, lambda node, dev: [Screen(hass, entry, node, dev)])


async def _send(entry: ConfigEntry, nodes: list[str], message: str, title: str | None) -> None:
    await entry.runtime_data._show(nodes, {"type": "_text", "text": message, "title": title or ""}, {"timeout": TIMEOUT})


class Screen(ShowEntity, NotifyEntity):
    _attr_icon = "mdi:message-badge-outline"
    _attr_supported_features = NotifyEntityFeature.TITLE

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, node: str, device_id: str) -> None:
        super().__init__(hass, node, device_id, "screen")
        self._entry = entry

    async def async_send_message(self, message: str, title: str | None = None) -> None:
        await _send(self._entry, [self.node], message, title)


class AllShows(NotifyEntity):
    _attr_has_entity_name = True
    _attr_translation_key = "all_screens"
    _attr_icon = "mdi:message-badge-outline"
    _attr_supported_features = NotifyEntityFeature.TITLE

    def __init__(self, entry: ConfigEntry) -> None:
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_all_screens"

    async def async_send_message(self, message: str, title: str | None = None) -> None:
        await _send(self._entry, self._entry.runtime_data.echos({}), message, title)
