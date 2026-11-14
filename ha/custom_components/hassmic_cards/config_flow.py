"""hassmic cards: set up from Settings → Devices & services (one entry), its options there; a YAML `hassmic_cards:` is
imported once.  The card library is picked from the storage dashboards there are (or the default, echo-cards, before
it is made); the LLM's
weather tools, a pattern few need to change, are in a collapsed section and checked."""
from __future__ import annotations

import re
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.core import HomeAssistant, callback
from homeassistant.data_entry_flow import section
from homeassistant.helpers import selector

from .const import CONF_ADVANCED, CONF_LIBRARY, CONF_ROOM_ENTITIES, CONF_TIMEOUT, CONF_VOICE, CONF_WEATHER_TOOLS, DEFAULTS, DOMAIN


def _dashboards(hass: HomeAssistant) -> list[str]:
    """the URLs of the storage dashboards there are (the library's candidates)"""
    try:
        from homeassistant.components.lovelace.const import LOVELACE_DATA
        data = hass.data.get(LOVELACE_DATA)
        return sorted(u for u, d in (data.dashboards.items() if data else []) if u and getattr(d, "mode", "") == "storage")
    except ImportError:
        return []


def _schema(hass: HomeAssistant, current: dict[str, Any]) -> vol.Schema:
    cur = {**DEFAULTS, **current}
    return vol.Schema({
        vol.Required(CONF_VOICE, default=cur[CONF_VOICE]): selector.BooleanSelector(),
        vol.Required(CONF_TIMEOUT, default=cur[CONF_TIMEOUT]): selector.NumberSelector(
            selector.NumberSelectorConfig(min=0, max=3600, step=1, unit_of_measurement="s", mode=selector.NumberSelectorMode.BOX)),
        vol.Required(CONF_ROOM_ENTITIES, default=cur[CONF_ROOM_ENTITIES]): selector.NumberSelector(
            selector.NumberSelectorConfig(min=1, max=8, step=1, mode=selector.NumberSelectorMode.SLIDER)),
        vol.Optional(CONF_LIBRARY, default=cur[CONF_LIBRARY]): selector.SelectSelector(
            selector.SelectSelectorConfig(options=_dashboards(hass), custom_value=True, mode=selector.SelectSelectorMode.DROPDOWN)),
        vol.Required(CONF_ADVANCED): section(vol.Schema({
            vol.Required(CONF_WEATHER_TOOLS, default=cur[CONF_WEATHER_TOOLS]): selector.TextSelector(),
        }), {"collapsed": True}),
    })


def _flat(data: dict[str, Any]) -> dict[str, Any]:
    data = {**data, **(data.get(CONF_ADVANCED) or {})}
    out = {**DEFAULTS, **{k: v for k, v in data.items() if k in DEFAULTS}}
    out[CONF_TIMEOUT], out[CONF_ROOM_ENTITIES] = int(out[CONF_TIMEOUT]), int(out[CONF_ROOM_ENTITIES])
    out[CONF_LIBRARY] = str(out[CONF_LIBRARY] or "").strip()
    out[CONF_VOICE] = bool(out[CONF_VOICE])
    return out


def _errors(hass: HomeAssistant, data: dict[str, Any]) -> dict[str, str]:
    errors = {}
    try:
        re.compile(data[CONF_WEATHER_TOOLS])
    except re.error:
        errors[CONF_ADVANCED] = "invalid_pattern"
    if data[CONF_LIBRARY] and data[CONF_LIBRARY] not in _dashboards(hass) and data[CONF_LIBRARY] != DEFAULTS[CONF_LIBRARY]:
        errors[CONF_LIBRARY] = "no_dashboard"           # (the default may be made later: shown cards then say so)
    return errors


class HassmicCardsFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        await self.async_set_unique_id(DOMAIN)
        self._abort_if_unique_id_configured()
        errors: dict[str, str] = {}
        if user_input is not None:
            data = _flat(user_input)
            if not (errors := _errors(self.hass, data)):
                return self.async_create_entry(title="Echo Show cards", data={}, options=data)
        return self.async_show_form(step_id="user", data_schema=_schema(self.hass, _flat(user_input or {})), errors=errors)

    async def async_step_import(self, import_data: dict[str, Any]) -> ConfigFlowResult:
        """configuration.yaml's hassmic_cards: once, as the entry's options (a bad pattern: the default)"""
        await self.async_set_unique_id(DOMAIN)
        self._abort_if_unique_id_configured()
        data = _flat(import_data or {})
        if "invalid_pattern" in _errors(self.hass, data).values():
            data[CONF_WEATHER_TOOLS] = DEFAULTS[CONF_WEATHER_TOOLS]
        return self.async_create_entry(title="Echo Show cards", data={}, options=data)

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return HassmicCardsOptions()


class HassmicCardsOptions(OptionsFlow):
    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            data = _flat(user_input)
            if not (errors := _errors(self.hass, data)):
                return self.async_create_entry(data=data)
        return self.async_show_form(step_id="init", data_schema=_schema(self.hass, _flat(user_input) if user_input else dict(self.config_entry.options)),
                                    errors=errors)
