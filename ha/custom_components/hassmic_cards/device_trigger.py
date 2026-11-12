"""Device triggers of an Echo Show: "a button on one of its cards pressed" (optionally only that action), for the
automation editor (the event hassmic_cards_action under it)."""
from __future__ import annotations

import voluptuous as vol

from homeassistant.components.device_automation import DEVICE_TRIGGER_BASE_SCHEMA
from homeassistant.components.homeassistant.triggers import event as event_trigger
from homeassistant.const import CONF_DEVICE_ID, CONF_DOMAIN, CONF_PLATFORM, CONF_TYPE
from homeassistant.core import CALLBACK_TYPE, HomeAssistant
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.trigger import TriggerActionType, TriggerInfo
from homeassistant.helpers.typing import ConfigType

from .const import ACTION_EVENT, DOMAIN

CONF_ACTION = "action"
TRIGGER_TYPES = {"card_button"}
TRIGGER_SCHEMA = DEVICE_TRIGGER_BASE_SCHEMA.extend({vol.Required(CONF_TYPE): vol.In(TRIGGER_TYPES),
                                                    vol.Optional(CONF_ACTION): cv.string})


def _echo(hass: HomeAssistant, device_id: str) -> str | None:
    """the Echo's ESPHome device for a device id (its own, or hassmic cards' device of it: the same MAC), from the
    registries alone (automations attach their triggers before hassmic cards may be set up)"""
    devreg = dr.async_get(hass)
    dev = devreg.async_get(device_id)
    if dev is None:
        return None
    esphome = [d for e in hass.config_entries.async_entries("esphome") for d in dr.async_entries_for_config_entry(devreg, e.entry_id)]
    if any(d.id == device_id for d in esphome):
        return device_id
    macs = {c for c in dev.connections if c[0] == dr.CONNECTION_NETWORK_MAC}
    return next((d.id for d in esphome if macs & set(d.connections)), None)


async def async_get_triggers(hass: HomeAssistant, device_id: str) -> list[dict[str, str]]:
    cards = hass.data.get(DOMAIN)
    if cards is None or cards.echo_device(device_id) is None:  # (an Echo Show's ESPHome device, or our device of it)
        return []
    return [{CONF_PLATFORM: "device", CONF_DOMAIN: DOMAIN, CONF_DEVICE_ID: device_id, CONF_TYPE: "card_button"}]


async def async_get_trigger_capabilities(hass: HomeAssistant, config: ConfigType) -> dict[str, vol.Schema]:
    return {"extra_fields": vol.Schema({vol.Optional(CONF_ACTION): str})}


async def async_attach_trigger(hass: HomeAssistant, config: ConfigType, action: TriggerActionType,
                               trigger_info: TriggerInfo) -> CALLBACK_TYPE:
    data = {CONF_DEVICE_ID: _echo(hass, config[CONF_DEVICE_ID]) or config[CONF_DEVICE_ID]}   # (a press's: the ESPHome device)
    if config.get(CONF_ACTION):
        data[CONF_ACTION] = config[CONF_ACTION]
    event_config = event_trigger.TRIGGER_SCHEMA({event_trigger.CONF_PLATFORM: "event", event_trigger.CONF_EVENT_TYPE: ACTION_EVENT,
                                                 event_trigger.CONF_EVENT_DATA: data})
    return await event_trigger.async_attach_trigger(hass, event_config, action, trigger_info, platform_type="device")
