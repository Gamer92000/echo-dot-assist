"""Adding an Echo: its address (or found on the network: the ESPHome mDNS hassmic announces says platform=hassmic), then
one press of its action button lets Home Assistant in, as a browser is let into its settings page."""
from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlowWithReload
from homeassistant.core import callback
from homeassistant.helpers import selector
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import DEFAULT_PORT, Echo, EchoError, new_key
from . import kiosk as k
from .const import CONF_KEY, CONF_MAC, CONF_NODE, DOMAIN

_LOGGER = logging.getLogger(__name__)


class EchoLocalConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    @staticmethod
    @callback
    def async_get_options_flow(entry: ConfigEntry) -> EchoLocalOptionsFlow:
        return EchoLocalOptionsFlow()

    def __init__(self) -> None:
        self.host: str | None = None
        self.port = DEFAULT_PORT
        self.key = new_key()
        self.hello: dict = {}
        self.asked = False

    def _echo(self) -> Echo:
        return Echo(async_get_clientsession(self.hass), self.host, self.port, self.key)

    async def _identify(self) -> None:
        """hello: who it is; one config entry per Echo, by its node (echo-show-5-5695c4)"""
        self.hello = await self._echo().get_hello()
        await self.async_set_unique_id(self.hello["node"])

    def _button(self) -> str:
        return "both volume keys together" if self.hello.get("action") == "volume_keys" else "the action button (the dot)"

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            self.host, self.port = user_input[CONF_HOST].strip(), user_input.get(CONF_PORT, DEFAULT_PORT)
            try:
                await self._identify()
            except EchoError:
                errors["base"] = "cannot_connect"
            else:
                self._abort_if_unique_id_configured(updates={CONF_HOST: self.host, CONF_PORT: self.port})
                return await self.async_step_approve()
        return self.async_show_form(step_id="user", errors=errors, data_schema=vol.Schema({
            vol.Required(CONF_HOST, default=self.host or ""): str,
            vol.Optional(CONF_PORT, default=self.port): int}))

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        if (discovery_info.properties.get("platform") or "").lower() != "hassmic":
            return self.async_abort(reason="not_hassmic")
        self.host = str(discovery_info.ip_address)
        try:
            await self._identify()
        except EchoError:
            return self.async_abort(reason="cannot_connect")
        self._abort_if_unique_id_configured(updates={CONF_HOST: self.host})
        self.context["title_placeholders"] = {"name": self.hello.get("name", self.host)}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return await self.async_step_approve()
        return self.async_show_form(step_id="confirm", description_placeholders={"name": self.hello.get("name", self.host)})

    async def async_step_approve(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """asks the Echo to let this key in (it plays its login sound and shows the blue ring), then waits for "Submit"
        after the press; a login runs out after a minute, asked again then"""
        errors: dict[str, str] = {}
        echo = self._echo()
        try:
            await echo.get_hello()
            state = await echo.login("Home Assistant")
        except EchoError:
            return self.async_abort(reason="cannot_connect")
        if state == "refused":             # another login at the same moment, or turned down: a new key, ask again
            self.key = new_key(); echo = self._echo(); await echo.get_hello(); state = await echo.login("Home Assistant")
            errors["base"] = "refused"
        if state == "approved":
            data = {CONF_HOST: self.host, "port": self.port, CONF_KEY: self.key, CONF_NODE: self.hello["node"],
                    CONF_MAC: self.hello.get("mac", ""), "name": self.hello.get("name", self.host)}
            if self.source == "reauth":
                return self.async_update_reload_and_abort(self._get_reauth_entry(), data=data)
            return self.async_create_entry(title=self.hello.get("name", self.host), data=data)
        if user_input is not None and not errors:
            errors["base"] = "not_approved"
        self.asked = True
        return self.async_show_form(step_id="approve", errors=errors,
                                    description_placeholders={"name": self.hello.get("name", self.host), "button": self._button()})

    async def async_step_reauth(self, entry_data: dict[str, Any]) -> ConfigFlowResult:
        """the Echo forgot this key (a factory reset, the login revoked on its page): let a new one in"""
        self.host, self.port = entry_data[CONF_HOST], entry_data.get("port", DEFAULT_PORT)
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """first a Submit: the login lights up the Echo and waits a minute for its button, so only when someone is there"""
        if user_input is None:
            return self.async_show_form(step_id="reauth_confirm", description_placeholders={"name": self._get_reauth_entry().title})
        try:
            await self._identify()
        except EchoError:
            return self.async_abort(reason="cannot_connect")
        return await self.async_step_approve()


class EchoLocalOptionsFlow(OptionsFlowWithReload):
    """The Echo Show's screen (kiosk.py): Home Assistant on it, which screens, where it starts.  The entry reloads on
    Submit, and its setup sends it all to the Echo."""

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        o = self.config_entry.options
        if user_input is not None:
            # Home's layout is the sidebar panel's (Echo Local › Home screen), and the Echo's own edit mode's
            return self.async_create_entry(data={**self.config_entry.options, **user_input})
        kinds = [selector.SelectOptionDict(value=v, label=l) for v, l in (
            ("wall", "Home: the home app's own (Echo-Wall's Home)"), ("dashboard", "A Home Assistant dashboard"),
            ("clock", "Clock: the home app's own (where it has one)"), ("custom", "A web page of your own"))]
        schema = vol.Schema({
            vol.Required(k.OPT_KIOSK, default=o.get(k.OPT_KIOSK, False)): bool,
            vol.Required(k.OPT_SCREENS, default=[x for x in o.get(k.OPT_SCREENS, ["wall", "dashboard"]) if x in k.SCREEN_KINDS]):
                selector.SelectSelector(selector.SelectSelectorConfig(options=kinds, multiple=True)),
            vol.Required(k.OPT_DEFAULT, default=o.get(k.OPT_DEFAULT) if o.get(k.OPT_DEFAULT) in k.SCREEN_KINDS else "wall"):
                selector.SelectSelector(selector.SelectSelectorConfig(options=kinds, mode=selector.SelectSelectorMode.DROPDOWN)),
            vol.Optional(k.OPT_DASHBOARD, default=o.get(k.OPT_DASHBOARD, "lovelace")): str,
            vol.Optional(k.OPT_CUSTOM_NAME, default=o.get(k.OPT_CUSTOM_NAME, "")): str,
            vol.Optional(k.OPT_CUSTOM_URL, default=o.get(k.OPT_CUSTOM_URL, "")): str,
            vol.Required(k.OPT_RETURN, default=o.get(k.OPT_RETURN, 120)):
                selector.NumberSelector(selector.NumberSelectorConfig(min=0, max=3600, step=10, unit_of_measurement="s", mode=selector.NumberSelectorMode.BOX)),
            vol.Required(k.OPT_ADMIN, default=o.get(k.OPT_ADMIN, False)): bool,
            vol.Optional(k.OPT_URL, default=o.get(k.OPT_URL, "")): str,
            vol.Optional(k.OPT_APP, default=o.get(k.OPT_APP, "")): str,
            vol.Optional(k.OPT_APP_SETTINGS, default=o.get(k.OPT_APP_SETTINGS, "")):
                selector.TextSelector(selector.TextSelectorConfig(multiline=True)),
        })
        return self.async_show_form(step_id="init", data_schema=schema,
                                    description_placeholders={"url": k.ha_url(self.hass, self.config_entry)})
