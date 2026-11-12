"""The logbook's words for hassmic cards' events: a card's button pressed, a card an Echo Show refused."""
from __future__ import annotations

from collections.abc import Callable

from homeassistant.components.logbook import LOGBOOK_ENTRY_MESSAGE, LOGBOOK_ENTRY_NAME
from homeassistant.core import Event, HomeAssistant, callback
from homeassistant.helpers import device_registry as dr

from .const import ACTION_EVENT, DOMAIN, REFUSED_EVENT


@callback
def async_describe_events(hass: HomeAssistant, async_describe_event: Callable) -> None:
    def name(event: Event) -> str:
        dev = dr.async_get(hass).async_get(event.data.get("device_id") or "")
        return (dev.name_by_user or dev.name) if dev else "Echo Show"

    @callback
    def action(event: Event) -> dict[str, str]:
        d = event.data
        return {LOGBOOK_ENTRY_NAME: name(event), LOGBOOK_ENTRY_MESSAGE: f"card button {d.get('action')} pressed (card {d.get('card')})"}

    @callback
    def refused(event: Event) -> dict[str, str]:
        return {LOGBOOK_ENTRY_NAME: name(event), LOGBOOK_ENTRY_MESSAGE: f"refused a card: {event.data.get('reason')}"}

    async_describe_event(DOMAIN, ACTION_EVENT, action)
    async_describe_event(DOMAIN, REFUSED_EVENT, refused)
