"""hassmic cards: cards on the Echo Shows' screens (echo-dot-assist's hassmic: its ESPHome actions show_card and
clear_card, and its events esphome.hassmic_card_action, _state, _refused). docs/cards.md has all of it; in short:

- Set up in Settings → Devices & services (options: the voice cards' timeout and rows, the card library's dashboard,
  the LLM's weather tools); a YAML `hassmic_cards:` is imported once.
- hassmic_cards.show: a dashboard card (entities, glance, tile, weather-forecast, markdown, picture-entity,
  picture-glance, picture, todo-list, calendar; the YAML a dashboard takes), or one of the card library's by name (a
  view of the library dashboard, its first card, with `variables` for its templates), on Echo Shows (the target;
  none: every Show). An id updates it in place and clears it; its entities' changes send it again. Buttons are
  "actions" as a phone notification has them ({action: ID, title}): a press fires hassmic_cards_action and the Echo's
  "Card button" event entity, and runs the button's own perform_action if it has one.
- Each Echo Show gets a "Card button" event entity and a "Card on screen" sensor (what shows; the last card it
  refused), and device triggers for its buttons. A refused card is in the logbook.
- hassmic_cards.show_message (a message from plain fields), hassmic_cards.clear; a notify entity per Show and one for all.
- Voice requests: the conversation's chat log has every tool call and its result. When the turn's answer comes (the
  assistant's content without tool calls), its card goes to the Echo that was asked (the assist satellite holding the
  conversation id) as a request's card: what was done (its controls, the rest of its room), else the weather, else
  what was asked about, else what went wrong in the assistant's own words. A new request in the conversation before the
  answer drops the turn.
"""
from __future__ import annotations

import logging
import re
import time
from typing import Any

import voluptuous as vol
from aiohttp import web

from homeassistant.components.http import HomeAssistantView
from homeassistant.config_entries import SOURCE_IMPORT, ConfigEntry
from homeassistant.components.homeassistant.exposed_entities import async_should_expose
from homeassistant.const import ATTR_AREA_ID, ATTR_DEVICE_ID, EVENT_SERVICE_REGISTERED, Platform
from homeassistant.core import Event, HomeAssistant, ServiceCall, ServiceResponse, SupportsResponse, callback
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.target import TargetSelection, async_extract_referenced_entity_ids

from .const import (ACTION_EVENT, CONF_LIBRARY, CONF_ROOM_ENTITIES, CONF_TIMEOUT, CONF_VOICE, CONF_WEATHER_TOOLS, DEFAULTS, DOMAIN,
                    ECHO_ACTION, ECHO_REFUSED, ECHO_STATE, REFUSED_EVENT, SIGNAL_DEVICE, SIGNAL_NEW_SHOW)
from .live import ACTION_SCHEMA, Live, short_id
from .render import CardError, Render

_LOGGER = logging.getLogger(__name__)
PLATFORMS = [Platform.EVENT, Platform.NOTIFY, Platform.SENSOR, Platform.SWITCH]

CONFIG_SCHEMA = vol.Schema(                     # (imported once into the config entry)
    {DOMAIN: vol.Schema({
        vol.Optional(CONF_TIMEOUT): vol.All(int, vol.Range(min=0, max=3600)),
        vol.Optional(CONF_ROOM_ENTITIES): vol.All(int, vol.Range(min=1, max=8)),
        vol.Optional(CONF_WEATHER_TOOLS): cv.string,
        vol.Optional(CONF_LIBRARY): cv.string,
        vol.Optional(CONF_VOICE): cv.boolean,
    })},
    extra=vol.ALLOW_EXTRA,
)

ACTIONS = {"HassTurnOn", "HassTurnOff", "HassToggle", "HassLightSet", "HassSetPosition", "HassClimateSetTemperature",
           "HassFanSetSpeed", "HassMediaPause", "HassMediaUnpause", "HassSetVolume", "HassOpen", "HassClose"}
QUERIES = {"HassGetState", "HassClimateGetTemperature"}
TURN_TTL = 120                      # s a turn waits for its answer (a leak guard: the answer normally ends it)

TARGET = {vol.Optional(ATTR_DEVICE_ID): vol.All(cv.ensure_list, [cv.string]),
          vol.Optional(ATTR_AREA_ID): vol.All(cv.ensure_list, [cv.string]),
          vol.Optional("entity_id"): cv.comp_entity_ids, vol.Optional("floor_id"): cv.ensure_list, vol.Optional("label_id"): cv.ensure_list}
SHOW_SCHEMA = vol.Schema({
    **TARGET,
    vol.Required("card"): vol.Any(cv.string, vol.All(dict, vol.Schema({vol.Required("type"): cv.string}, extra=vol.ALLOW_EXTRA))),
    vol.Optional("variables", default={}): dict,
    vol.Optional("id"): short_id,
    vol.Optional("title"): cv.string, vol.Optional("subtitle"): cv.string, vol.Optional("message"): cv.string,
    vol.Optional("timeout", default=30): vol.All(vol.Coerce(int), vol.Range(min=0, max=86400)),
    vol.Optional("scope", default="notice"): vol.In(["notice", "request"]),
    vol.Optional("conversation_id"): vol.Any(None, cv.string),
    vol.Optional("actions", default=[]): vol.All(cv.ensure_list, vol.Length(max=3), [ACTION_SCHEMA]),
})
CLEAR_SCHEMA = vol.Schema({**TARGET, vol.Optional("id"): short_id})
MESSAGE_SCHEMA = vol.Schema({                   # show_message: a message card from plain fields (the automation editor's)
    **TARGET,
    vol.Optional("title", default=""): cv.string, vol.Optional("message", default=""): cv.string,
    vol.Optional("subtitle"): cv.string,
    vol.Optional("camera"): cv.entity_domain("camera"), vol.Optional("image"): cv.string,
    vol.Optional("live", default=True): cv.boolean,
    vol.Optional("id"): short_id,
    vol.Optional("timeout", default=30): vol.All(vol.Coerce(int), vol.Range(min=0, max=86400)),
    **{vol.Optional(f"button_{i}"): cv.string for i in (1, 2, 3)},
    **{vol.Optional(f"button_{i}_action"): vol.Any("", short_id) for i in (1, 2, 3)},   # ("": made from the id)
    **{vol.Optional(f"button_{i}_snooze"): vol.All(vol.Coerce(float), vol.Range(min=0, max=1440)) for i in (1, 2, 3)},
})


def _error(key: str, **placeholders: str) -> ServiceValidationError:
    """a refusal in the person's language (translations: exceptions)"""
    return ServiceValidationError(translation_domain=DOMAIN, translation_key=key, translation_placeholders=placeholders)


def _intent(tool_name: str) -> str:
    """the intent behind a tool: the LLM's tools carry a prefix ("intent__HassTurnOff", "homeassistant__GetLiveContext")"""
    return tool_name.split("__", 1)[1] if "__" in tool_name else tool_name


class Cards:
    def __init__(self, hass: HomeAssistant, conf: dict[str, Any]) -> None:
        conf = {**DEFAULTS, **conf}
        self.hass = hass
        self.timeout = int(conf[CONF_TIMEOUT])
        self.room_entities = int(conf[CONF_ROOM_ENTITIES])
        self.voice = bool(conf[CONF_VOICE])
        self.voice_off: set[str] = set()                        # Shows whose "Voice cards" switch is off (their nodes)
        try:
            self.weather_tools = re.compile(conf[CONF_WEATHER_TOOLS], re.I)
        except re.error:                                        # (the options flow checks it; a YAML import may not)
            _LOGGER.warning("LLM weather tools %r: not a pattern, the default taken", conf[CONF_WEATHER_TOOLS])
            self.weather_tools = re.compile(DEFAULTS[CONF_WEATHER_TOOLS], re.I)
        self.render = Render(hass, conf[CONF_LIBRARY])
        self.live = Live(hass, self.render)
        self.state: dict[str, dict] = {}                        # device id: what its screen shows, its last refusal
        self.calls: dict[str, tuple[str, dict]] = {}            # tool_call_id: (tool name, args)
        self.turns: dict[str, dict] = {}                        # conversation id: what this turn did, until its answer
        self.voice_ok = True

    # ------------------------------------------------------------------ the Echos

    def _node(self, entry) -> str | None:
        """an ESPHome config entry's node name, if that device is an Echo Show running hassmic (it has show_card)"""
        try:
            name = entry.runtime_data.device_info.name.replace("-", "_")
        except AttributeError:
            return None
        return name if self.hass.services.has_service("esphome", f"{name}_show_card") else None

    def node_of_device(self, device_id: str) -> str | None:
        """the node of an Echo Show's device: its ESPHome device, or hassmic cards' own one of it"""
        shows = self.shows()
        target = self.echo_device(device_id, shows)
        return next((n for n, dev in shows if dev == target), None) if target else None

    def echo_device(self, device_id: str | None, shows_now: list[tuple[str, str]] | None = None) -> str | None:
        """the ESPHome device of an Echo Show, from its own id or from hassmic cards' device of it (the same MAC)"""
        if not device_id:
            return None
        shows = {dev for _, dev in (self.shows() if shows_now is None else shows_now)}
        if device_id in shows:
            return device_id
        devreg = dr.async_get(self.hass)
        mine = devreg.async_get(device_id)
        macs = {c for c in (mine.connections if mine else ()) if c[0] == dr.CONNECTION_NETWORK_MAC}
        return next((d for d in shows if macs & set(getattr(devreg.async_get(d), "connections", ()))), None)

    def shows(self) -> list[tuple[str, str]]:
        """(node, device id) of every Echo Show running hassmic Home Assistant knows"""
        devreg, out = dr.async_get(self.hass), []
        for e in self.hass.config_entries.async_entries("esphome"):
            if not (n := self._node(e)):
                continue
            info = getattr(getattr(e, "runtime_data", None), "device_info", None)
            mac = dr.format_mac(info.mac_address) if getattr(info, "mac_address", None) else None
            devs = dr.async_entries_for_config_entry(devreg, e.entry_id)       # (the Echo's own: the one with its MAC)
            dev = next((d for d in devs if mac and (dr.CONNECTION_NETWORK_MAC, mac) in d.connections), None) or next(iter(devs), None)
            if dev:
                out.append((n, dev.id))
        return out

    def echos(self, data: dict) -> list[str]:
        """the Echo Shows an action's target names (devices; areas, floors, labels: their Shows; entities: their devices'),
        none targeted: every Show.  A target that names no Show is an error, not every Show"""
        sel = TargetSelection(data)
        if not sel.has_any_target:
            return [n for e in self.hass.config_entries.async_entries("esphome") if (n := self._node(e))]
        ref = async_extract_referenced_entity_ids(self.hass, sel)
        devreg, entreg = dr.async_get(self.hass), er.async_get(self.hass)
        devs = set(ref.referenced_devices)
        for area in ref.referenced_areas:
            devs |= {d.id for d in dr.async_entries_for_area(devreg, area)}
        for eid in ref.referenced | ref.indirectly_referenced:
            if (ent := entreg.async_get(eid)) and ent.device_id:
                devs.add(ent.device_id)
        shows = self.shows()                                    # (once: each lookup would walk the registries again)
        echo_devs = {self.echo_device(d, shows) for d in devs}
        out = sorted({n for n, dev in shows if dev in echo_devs})
        if not out:
            raise _error("no_show")
        return out

    def _satellite(self, conversation_id: str):
        """the assist satellite whose run this conversation is (it stores the run's conversation id before it starts)"""
        comp = self.hass.data.get("assist_satellite")
        for ent in list(getattr(comp, "entities", []) or []):
            if getattr(ent, "_conversation_id", None) == conversation_id and ent.platform and ent.platform.platform_name == "esphome":
                return ent
        return None

    def default_weather(self) -> str | None:
        """the weather entity for a card nobody named one for: one exposed to Assist and there, else any that is there"""
        ids = list(self.hass.states.async_entity_ids("weather"))
        ok = [e for e in ids if self.hass.states.get(e).state not in ("unavailable", "unknown")]
        exposed = [e for e in ok if async_should_expose(self.hass, "conversation", e)]
        return (exposed or ok or ids or [None])[0]

    # ------------------------------------------------------------------ voice requests

    @callback
    def on_chat_log(self, conversation_id: str, event_type: str, data: dict) -> None:
        """called inside the conversation's own turn: whatever goes wrong here must never reach it (an exception would
        end the voice request with "Unexpected error during intent recognition")"""
        try:
            self._on_chat_log(conversation_id, event_type, data)
        except Exception:                                       # noqa: BLE001
            _LOGGER.exception("chat log event of %s: no card", conversation_id)

    def _on_chat_log(self, conversation_id: str, event_type: str, data: dict) -> None:
        if str(event_type) != "content_added" or not self.voice_ok:
            return
        now = time.monotonic()
        for cid in [k for k, t in self.turns.items() if now - t["at"] > TURN_TTL]:
            self.turns.pop(cid, None)
        c = (data or {}).get("content") or {}
        role = c.get("role")
        if role == "user":                                      # a new request: an earlier turn left without its answer goes
            self.turns.pop(conversation_id, None)
            return
        for call in c.get("tool_calls") or []:                  # remembered: GetLiveContext's result needs its args
            get = (lambda k, d=None, call=call: call.get(k, d)) if isinstance(call, dict) else (lambda k, d=None, call=call: getattr(call, k, d))
            if get("id"):                                       # (llm.ToolInput objects in content_added, dicts elsewhere)
                self.calls[get("id")] = (get("tool_name", ""), get("tool_args") or {})
                if len(self.calls) > 200:
                    self.calls.pop(next(iter(self.calls)))
        if role == "assistant" and not c.get("tool_calls"):    # the answer: the turn is over, its card now
            turn = self.turns.pop(conversation_id, None)
            if turn:
                turn["answer"] = c.get("content") or ""
                self.hass.async_create_task(self._turn_done_safe(conversation_id, turn), eager_start=False)
            return
        if role != "tool_result":
            return
        intent = _intent(c.get("tool_name", ""))
        result = c.get("tool_result") or {}
        args = self.calls.pop(c.get("tool_call_id", ""), ("", {}))[1]
        data_ = result.get("data") if isinstance(result, dict) else None
        success = [t["id"] for t in (data_ or {}).get("success") or [] if t.get("type") == "entity" and t.get("id")]
        failed = [t.get("name") or t.get("id") for t in (data_ or {}).get("failed") or []]
        turn = self.turns.setdefault(conversation_id, {"focus": [], "asked": [], "weather": None, "failed": False, "at": now})
        if intent == "HassGetWeather":
            turn["weather"] = next((e for e in success if e.startswith("weather.")), None) or self.default_weather()
        elif intent in ACTIONS:
            turn["focus"] += success; turn["failed"] = turn["failed"] or bool(failed)
        elif intent in QUERIES:
            turn["asked"] += success
        elif intent == "GetLiveContext" and args:                # the LLM asked about something by name, area or domain
            turn["asked"] += self._live_context(result)
        elif self.weather_tools.search(intent) and not (isinstance(result, dict) and result.get("error")):
            turn["weather"] = turn["weather"] or self.default_weather()

    def _live_context(self, result: Any) -> list[str]:
        """GetLiveContext's overview (YAML-ish text: "- names: X") back to entity ids, by name or alias, exposed ones only"""
        text = result.get("result", "") if isinstance(result, dict) else str(result)
        names = [n.strip().strip("'\"") for n in re.findall(r"^\s*-\s*names:\s*(.+)$", text, re.M)]
        if not names or len(names) > 8:
            return []
        entreg, out = er.async_get(self.hass), []
        for st in self.hass.states.async_all():
            if not async_should_expose(self.hass, "conversation", st.entity_id):
                continue
            ent = entreg.async_get(st.entity_id)
            known = {st.name} | set(ent.aliases if ent else ())
            if any(n in known for n in names):
                out.append(st.entity_id)
        return out[:8]

    async def _turn_done_safe(self, conversation_id: str, turn: dict) -> None:
        try:
            await self._turn_done(conversation_id, turn)
        except Exception:                                       # noqa: BLE001
            _LOGGER.exception("conversation %s: no card", conversation_id)

    async def _turn_done(self, conversation_id: str, turn: dict) -> None:
        sat = self._satellite(conversation_id)
        if sat is None:
            _LOGGER.debug("conversation %s: no assist satellite of an Echo Show runs it", conversation_id)
            return
        node = self._node(sat.platform.config_entry)
        if node is None or not self.voice or node in self.voice_off:
            return                                              # an Echo without a screen (or cards), voice cards off
        asked = [e for e in dict.fromkeys(turn["asked"]) if not e.startswith("weather.")]
        weather = turn["weather"] or next((e for e in turn["asked"] if e.startswith("weather.")), None)
        opts = {"scope": "request", "conversation_id": conversation_id, "timeout": self.timeout}
        cfg = None
        if turn["focus"]:                                       # done: its controls, and the rest of its room
            focus = list(dict.fromkeys(turn["focus"]))
            cfg = {"type": "entities", "entities": [{"entity": e, "focus": True} for e in focus], "_room": True,
                   "_rows": self.room_entities}
        elif weather:
            cfg = {"type": "weather-forecast", "entity": weather, "name": ""}
        elif asked:
            cfg = {"type": "entities", "entities": asked, "_rows": self.room_entities}
        elif turn["failed"] and turn.get("answer"):             # nothing done: what went wrong, in the answer's words
            cfg = {"type": "_text", "text": turn["answer"]}     # (as said: never rendered as a template)
        if cfg is None:
            return
        try:
            await self.live.show([node], cfg, opts)
        except CardError as err:
            _LOGGER.debug("conversation %s: no card: %s", conversation_id, err)

    # ------------------------------------------------------------------ actions

    async def svc_show(self, call: ServiceCall) -> ServiceResponse:
        d = call.data
        opts = {k: d[k] for k in ("id", "title", "subtitle", "message", "timeout", "scope", "conversation_id") if d.get(k) is not None}
        opts["actions"] = [dict(a) for a in d.get("actions") or []]
        card = d["card"]
        cfg = {"type": "_library", "name": card, "variables": dict(d.get("variables") or {})} if isinstance(card, str) \
            else {**card, "_variables": dict(d.get("variables") or {})} if d.get("variables") else dict(card)
        return await self._show(self.echos(d), cfg, opts)

    async def _show(self, echos: list[str], cfg: dict, opts: dict) -> ServiceResponse:
        try:
            result = await self.live.show(echos, cfg, opts)
        except CardError as err:
            raise _error("card", error=str(err)) from err
        return result or {"id": None, "shown_on": [], "failed": []}

    async def svc_message(self, call: ServiceCall) -> ServiceResponse:
        """a message card from plain fields: a picture (a camera's, live or not; an image), up to 3 buttons"""
        d = call.data
        if d.get("camera"):
            cfg = {"type": "picture-entity", "entity": d["camera"], "show_name": False, **({"camera_view": "live"} if d["live"] else {})}
        elif d.get("image"):
            cfg = {"type": "picture", "image": d["image"]}
        else:
            cfg = {"type": "_text", "text": d.get("message", "")}
        opts = {k: d[k] for k in ("id", "title", "subtitle", "timeout") if d.get(k) not in (None, "")}
        if cfg["type"] != "_text":
            opts["message"] = d.get("message", "")
        else:
            cfg["title"] = d.get("title", "")
        base = re.sub(r"[^A-Za-z0-9]+", "_", str(d.get("id") or d.get("title") or "card")).strip("_").upper()[:56] or "CARD"
        opts["actions"] = [{"action": d.get(f"button_{i}_action") or f"{base}_{i}", "title": d[f"button_{i}"],
                            **({"snooze": d[f"button_{i}_snooze"]} if d.get(f"button_{i}_snooze") is not None else {})}
                           for i in (1, 2, 3) if d.get(f"button_{i}")]
        if not (d.get("title") or d.get("message") or d.get("camera") or d.get("image")):
            raise _error("empty_message")
        return await self._show(self.echos(d), cfg, opts)

    async def svc_clear(self, call: ServiceCall) -> None:
        targeted = TargetSelection(call.data).has_any_target
        cid = call.data.get("id")
        known = cid in self.live.cards
        await self.live.clear(cid, self.echos(call.data) if targeted or not known else None)   # (an id we no longer
                                                                   # know: every Show, or the target's)


    # ------------------------------------------------------------------ the Echos' events

    async def on_echo_event(self, event: Event) -> None:
        """the Echo's events: a button (its card's own Echos only: live.py), what its screen shows, a card it refused"""
        device_id = event.data.get(ATTR_DEVICE_ID)
        node = self.node_of_device(device_id) if device_id else None
        if node is None:                                        # (any ESPHome device may fire esphome.* events)
            _LOGGER.debug("%s from a device that is no Echo Show (%s): ignored", event.event_type, device_id)
            return
        if event.event_type == ECHO_ACTION:
            entry = self.live.cards.get(event.data.get("card", ""))
            foreign = entry is not None and node not in entry["echos"]
            await self.live.on_action(event, self.node_of_device)
            if not foreign:                                     # (live.py ignores such a press: so does its entity)
                async_dispatcher_send(self.hass, SIGNAL_DEVICE.format(device_id), "action", dict(event.data))
        elif event.event_type == ECHO_STATE:
            cid, shown = event.data.get("card", ""), event.data.get("state") == "shown"
            entry = self.live.cards.get(cid)
            if shown and entry is not None:
                self.live.extend(cid)                           # (kept, and followed, while it shows)
            st = self.state.setdefault(device_id, {})
            st.update(card=cid, title=event.data.get("title", ""), shown=shown,
                      named=bool(entry["named"]) if entry is not None else bool(cid),
                      scope=(entry or {}).get("opts", {}).get("scope", "notice") if shown else "")
            async_dispatcher_send(self.hass, SIGNAL_DEVICE.format(device_id), "state", dict(st))
        elif event.event_type == ECHO_REFUSED:
            reason = event.data.get("reason", "")
            if reason.endswith("not shown (gone)") or reason.startswith("cards are switched off"):
                _LOGGER.debug("%s", reason)                      # an update of a card no longer there, a card to an Echo
                return                                          # whose cards just went off: as meant
            st = self.state.setdefault(device_id, {})
            st["refused"] = reason
            _LOGGER.warning("an Echo Show (%s) refused a card: %s", self.node_of_device(device_id) or device_id, reason)
            self.hass.bus.async_fire(REFUSED_EVENT, {"reason": reason, ATTR_DEVICE_ID: device_id}, context=event.context)
            async_dispatcher_send(self.hass, SIGNAL_DEVICE.format(device_id), "refused", dict(st))


class CameraView(HomeAssistantView):
    """a card's camera picture for the Echos: no login, the token is the card's own and ends with it"""
    url = "/api/hassmic_cards/camera/{token}"
    name = "api:hassmic_cards:camera"
    requires_auth = False

    CACHE_S = 1.0                       # one snapshot a camera this long: Shows refreshing the same card share it

    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass
        self._cache: dict[str, tuple[float, Any]] = {}

    async def get(self, request: web.Request, token: str) -> web.Response:
        cards = self.hass.data.get(DOMAIN)
        pic = cards.live.pictures.get(token) if isinstance(cards, Cards) else None
        if not pic or pic[1] < time.time():
            return web.Response(status=404)
        from homeassistant.components.camera import async_get_image
        cached = self._cache.get(pic[0])
        if cached and time.monotonic() - cached[0] < self.CACHE_S:
            img = cached[1]
        else:
            try:
                img = await async_get_image(self.hass, pic[0], timeout=10, width=800)
            except HomeAssistantError as err:
                _LOGGER.debug("picture of %s: %s", pic[0], err)
                return web.Response(status=502)
            self._cache[pic[0]] = (time.monotonic(), img)
        return web.Response(body=img.content, content_type=img.content_type, headers={"Cache-Control": "no-store"})


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """the actions and the camera view (once, whatever the entry does); configuration.yaml's hassmic_cards imported"""
    if DOMAIN in config:
        hass.async_create_task(hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_IMPORT}, data=dict(config[DOMAIN] or {})))
        ir.async_create_issue(hass, DOMAIN, "yaml", is_fixable=False, severity=ir.IssueSeverity.WARNING, translation_key="yaml")

    def cards() -> Cards:
        if not isinstance(c := hass.data.get(DOMAIN), Cards):
            raise _error("not_set_up")
        return c

    async def show(call: ServiceCall) -> ServiceResponse: return await cards().svc_show(call)
    async def message(call: ServiceCall) -> ServiceResponse: return await cards().svc_message(call)
    async def clear(call: ServiceCall) -> None: await cards().svc_clear(call)
    hass.services.async_register(DOMAIN, "show", show, schema=SHOW_SCHEMA, supports_response=SupportsResponse.OPTIONAL)
    hass.services.async_register(DOMAIN, "show_message", message, schema=MESSAGE_SCHEMA, supports_response=SupportsResponse.OPTIONAL)
    hass.services.async_register(DOMAIN, "clear", clear, schema=CLEAR_SCHEMA)
    hass.http.register_view(CameraView(hass))
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    cards = Cards(hass, dict(entry.options))
    hass.data[DOMAIN] = cards
    entry.runtime_data = cards
    await cards.live.async_load()
    # what voice cards rely on: both are HA internals, checked here
    try:
        from homeassistant.components.assist_satellite.entity import AssistSatelliteEntity
        from homeassistant.components.conversation.chat_log import async_subscribe_chat_logs
        ok = hasattr(AssistSatelliteEntity, "_conversation_id")
    except ImportError:
        ok = False
    if ok:
        entry.async_on_unload(async_subscribe_chat_logs(hass, cards.on_chat_log))
        ir.async_delete_issue(hass, DOMAIN, "voice_cards")
    else:
        cards.voice_ok = False
        ir.async_create_issue(hass, DOMAIN, "voice_cards", is_fixable=False, severity=ir.IssueSeverity.WARNING,
                              translation_key="voice_cards")
        _LOGGER.error("voice cards off: this Home Assistant has no chat log subscription or satellite conversation id; "
                      "hassmic_cards.show still works")

    async def echo_event(event: Event) -> None:
        try:
            await cards.on_echo_event(event)
        except Exception:                                       # noqa: BLE001
            _LOGGER.exception("an Echo's card event: %s", event.data)
    for ev in (ECHO_ACTION, ECHO_STATE, ECHO_REFUSED):
        entry.async_on_unload(hass.bus.async_listen(ev, echo_event))

    @callback
    def service_registered(event: Event) -> None:               # an Echo Show adopted (or back): its entities
        if event.data.get("domain") == "esphome" and str(event.data.get("service", "")).endswith("_show_card"):
            async_dispatcher_send(hass, SIGNAL_NEW_SHOW)
    entry.async_on_unload(hass.bus.async_listen(EVENT_SERVICE_REGISTERED, service_registered))
    entry.async_on_unload(entry.add_update_listener(_reload))
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_remove_config_entry_device(hass: HomeAssistant, entry: ConfigEntry, device: dr.DeviceEntry) -> bool:
    """a device of ours whose Echo Show is gone (no hassmic, removed from Home Assistant) may be deleted"""
    cards = getattr(entry, "runtime_data", None)
    return cards is None or cards.echo_device(device.id) is None


async def _reload(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if ok:
        await entry.runtime_data.live.async_shutdown()
        hass.data.pop(DOMAIN, None)
    return ok
