"""The cards hassmic_cards has out on Echo Shows: by id, kept up to date, their buttons and pictures; kept over a restart.

A card stays here while it may show on an Echo: its timeout and a margin, or LIVE_MAX for one until tapped away, again
from each time an Echo reports it showing (one until tapped away with buttons then keeps only its buttons, BUTTONS_MAX:
the Echo keeps it until tapped, and "Remind me" the next morning must still work) (the Echo reports what it shows, not what it keeps under a newer notice; an
update for a card that is gone is refused there and costs nothing). While it is here:
- a change of one of its entities sends it again (debounced), "update": the Echo replaces it in place;
- a button press (the Echo's event esphome.hassmic_card_action, from an Echo the card is on) fires hassmic_cards_action
  and runs the button's own action if it has one; a button that takes its card away takes it off every Echo it is on,
  and runs once; a button with "snooze" takes it away and brings it back that many minutes later (kept over a restart);
- its camera picture is served at its own token (/api/hassmic_cards/camera/<token>), which ends with it.
"""
from __future__ import annotations

import json
import logging
import re
import secrets
import time
from typing import Any, Callable

import voluptuous as vol

from homeassistant.const import EVENT_HOMEASSISTANT_STARTED
from homeassistant.core import CoreState, Event, HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.event import async_call_later, async_track_state_change_event
from homeassistant.helpers.network import NoURLAvailableError, get_url
from homeassistant.helpers.storage import Store

from .render import CardError, Render

_LOGGER = logging.getLogger(__name__)

from .const import ACTION_EVENT

CARDS_VERSION = 2                    # the Echo's card format (hassmic's cards.h CARDS_VERSION)
CARD_JSON_MAX = 8000                 # what the Echo takes (cards.h CARD_JSON_MAX 8192), a margin under it
LIVE_MAX = 12 * 3600                 # s a card until tapped away is kept up to date
BUTTONS_MAX = 30 * 86400             # s such a card's buttons are answered after that (no updates, no picture)
ANSWERED_S = 10                      # s a card answered by a button stays answered: a press on its other Show meanwhile
REQUEST_MARGIN, NOTICE_MARGIN = 60, 30
DEBOUNCE = 1.0
def short_id(value: Any) -> str:
    """a card's or a button's id: the Echo keeps 63 bytes of it (a longer one would come back cut in its events)"""
    value = cv.string(value)
    if not value or len(value.encode()) > 63:
        raise vol.Invalid("an id is 1 to 63 bytes")
    return value


ACTION_SCHEMA = vol.Schema({                           # a button, as a phone notification's action
    vol.Required("action"): short_id, vol.Required("title"): cv.string,
    vol.Optional("perform_action"): cv.service, vol.Optional("data", default={}): dict,
    vol.Optional("keep", default=False): cv.boolean,
    vol.Optional("snooze"): vol.All(vol.Coerce(float), vol.Range(min=0, max=1440)),   # minutes: the card back after them
})


class Live:
    def __init__(self, hass: HomeAssistant, render: Render) -> None:
        self.hass = hass
        self.render = render
        self.cards: dict[str, dict] = {}
        self.pictures: dict[str, tuple[str, float]] = {}       # token: (camera entity, wall-clock end)
        self._unsub: dict[str, Callable] = {}
        self._pending: dict[str, Callable] = {}
        self._ends: dict[str, Callable] = {}
        self._store = Store(hass, 1, "hassmic_cards.cards")
        self._answered: dict[str, float] = {}                   # card id: when a button took it away (monotonic)

    # ------------------------------------------------------------------ keeping

    async def async_load(self) -> None:
        data = await self._store.async_load() or {}
        now = time.time()
        self.pictures = {t: (c, e) for t, (c, e) in (data.get("pictures") or {}).items() if e > now}
        for entry in data.get("cards") or []:
            if entry.get("snoozed_until"):                       # a reminder due: kept until it is shown again
                self.cards[entry["id"]] = entry
                self._schedule_return(entry["id"])
            elif entry.get("expires", 0) > now:
                self.cards[entry["id"]] = entry
                self._schedule_end(entry["id"])

        # their entities followed again once Home Assistant runs (at setup the entities of other integrations, the to-do
        # and calendar services, may not be there yet: a card rendered then would follow nothing)
        async def retrack(_event: Event | None = None) -> None:
            for cid in list(self.cards):
                await self._retrack(cid)
        if self.hass.state is CoreState.running:
            self.hass.async_create_task(retrack(), eager_start=False)
        else:
            self.hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STARTED, retrack)

    def _data(self) -> dict:
        return {"cards": list(self.cards.values()), "pictures": {t: list(v) for t, v in self.pictures.items()}}

    def _save(self) -> None:
        self._store.async_delay_save(self._data, 2)

    async def _retrack(self, cid: str) -> None:
        entry = self.cards.get(cid)
        if not entry:
            return
        try:
            _, ents = await self.render.card(entry["cfg"])
        except CardError:
            ents = set()
        self._track(cid, ents)

    def _track(self, cid: str, ents: set[str]) -> None:
        if (old := self._unsub.pop(cid, None)):
            old()
        if not ents:
            return

        @callback
        def changed(_event: Event) -> None:
            if cid in self._pending:
                return

            async def later(_now: Any) -> None:
                self._pending.pop(cid, None)
                await self.send(cid, update=True)
            self._pending[cid] = async_call_later(self.hass, DEBOUNCE, later)
        self._unsub[cid] = async_track_state_change_event(self.hass, list(ents), changed)

    def _schedule_end(self, cid: str) -> None:
        if (old := self._ends.pop(cid, None)):
            old()
        entry = self.cards[cid]

        async def end(_now: Any) -> None:
            self._ends.pop(cid, None)
            e = self.cards.get(cid)
            if e and not int(e["opts"].get("timeout", 30)) and e["opts"].get("actions") and not e.get("stale"):
                e["stale"] = True                               # buttons only: nothing followed, its picture ended
                e["expires"] = time.time() + BUTTONS_MAX
                for d in (self._unsub, self._pending):
                    if (f := d.pop(cid, None)):
                        f()
                for t in e.get("pictures") or []:
                    self.pictures.pop(t, None)
                self._schedule_end(cid)
                self._save()
                return
            self.forget(cid)
        self._ends[cid] = async_call_later(self.hass, max(1.0, entry["expires"] - time.time()), end)

    async def async_shutdown(self) -> None:
        """the entry unloaded: nothing followed, nothing pending, what is kept saved now (the next entry loads it)"""
        for d in (self._unsub, self._pending, self._ends):
            for f in d.values():
                f()
            d.clear()
        await self._store.async_save(self._data())

    def extend(self, cid: str) -> None:
        """an Echo shows the card (again: back from under a newer notice, a reply that took long): kept from now on"""
        entry = self.cards.get(cid)
        if entry is None or entry.get("snoozed_until"):
            return
        timeout = int(entry["opts"].get("timeout", 30))
        margin = REQUEST_MARGIN if entry["opts"].get("scope") == "request" else NOTICE_MARGIN
        if entry.pop("stale", None):                            # shown again after it kept only its buttons: live again
            entry["expires"] = 0
            self.hass.async_create_task(self._retrack(cid), eager_start=False)
        entry["expires"] = max(entry["expires"], time.time() + (timeout + margin if timeout else LIVE_MAX))
        for t in entry.get("pictures") or []:
            if t in self.pictures:
                self.pictures[t] = (self.pictures[t][0], entry["expires"])
        self._schedule_end(cid)

    def _schedule_return(self, cid: str) -> None:
        if (old := self._ends.pop(cid, None)):
            old()
        entry = self.cards[cid]

        async def back(_now: Any) -> None:
            self._ends.pop(cid, None)
            entry.pop("snoozed_until", None)
            entry.pop("stale", None)
            timeout = int(entry["opts"].get("timeout", 30))
            entry["expires"] = time.time() + (timeout + NOTICE_MARGIN if timeout else LIVE_MAX)
            self._schedule_end(cid)
            await self._retrack(cid)
            await self.send(cid)
            self._save()
        self._ends[cid] = async_call_later(self.hass, max(1.0, entry["snoozed_until"] - time.time()), back)

    def forget(self, cid: str) -> None:
        entry = self.cards.pop(cid, None)
        for d in (self._unsub, self._pending, self._ends):
            if (f := d.pop(cid, None)):
                f()
        if entry:
            for t in entry.get("pictures") or []:
                self.pictures.pop(t, None)
            self._save()

    # ------------------------------------------------------------------ out to the Echos

    def _base_url(self) -> str:
        try:
            return get_url(self.hass, allow_external=False, allow_cloud=False)
        except NoURLAvailableError:
            return get_url(self.hass)

    async def show(self, echos: list[str], cfg: dict, opts: dict) -> dict | None:
        """a card for these Echos (node names); its id (given, or made); raises CardError for a card that cannot be"""
        if not echos:
            _LOGGER.debug("no Echo Show to show %s on", cfg.get("type"))
            return None
        cid = str(opts.get("id") or secrets.token_hex(6))
        device, ents = await self.render.card(cfg)                 # (raises before anything changes)
        old = self.cards.get(cid)
        timeout = int(opts.get("timeout", 30))
        margin = REQUEST_MARGIN if opts.get("scope") == "request" else NOTICE_MARGIN
        entry = {"id": cid, "named": bool(opts.get("id")), "echos": list(dict.fromkeys(echos)), "cfg": cfg, "opts": opts,
                 "expires": time.time() + (timeout + margin if timeout else LIVE_MAX),
                 "pictures": (old or {}).get("pictures") or []}
        if old and set(old["echos"]) - set(entry["echos"]):         # the same id, fewer Echos: off the ones it left
            await self._clear_on(sorted(set(old["echos"]) - set(entry["echos"])), cid)
        self.cards[cid] = entry
        self._track(cid, ents)
        self._schedule_end(cid)
        result = await self.send(cid, device=device)
        self._save()
        return {"id": cid, **result}

    async def send(self, cid: str, device: dict | None = None, update: bool = False) -> dict:
        """the card out to its Echos: which took it (the action sent), which did not (gone from Home Assistant)"""
        entry = self.cards.get(cid)
        if not entry:
            return {"shown_on": [], "failed": []}
        if device is None:
            try:
                device, ents = await self.render.card(entry["cfg"])
            except CardError as err:
                _LOGGER.debug("card %s: no update: %s", cid, err)
                return {"shown_on": [], "failed": []}
            if cid not in self._unsub:
                self._track(cid, ents)
        payload = json.dumps(self._echo_card(entry, device, update), ensure_ascii=False)
        _LOGGER.debug("card for %s: %s", entry["echos"], re.sub(r"(/api/hassmic_cards/camera/)[\w-]+", r"\1…", payload))
        shown, failed = [], []
        for node in entry["echos"]:
            if not self.hass.services.has_service("esphome", f"{node}_show_card"):
                failed.append(node)                             # (an Echo whose cards are off, or gone from Home Assistant)
                continue
            try:
                await self.hass.services.async_call("esphome", f"{node}_show_card", {"card": payload}, blocking=True)
                shown.append(node)
            except Exception as err:                            # noqa: BLE001  (an Echo offline: the others still get theirs)
                failed.append(node)
                (_LOGGER.debug if update else _LOGGER.warning)("show_card on %s: %s", node, err)
        return {"shown_on": shown, "failed": failed}

    def _echo_card(self, entry: dict, device: dict, update: bool) -> dict:
        opts = entry["opts"]
        c: dict[str, Any] = {"v": CARDS_VERSION, "id": entry["id"]}
        if update:
            c["update"] = True
        c.update({k: v for k, v in device.items() if k not in ("image", "count")})
        if opts.get("scope") == "request":
            c["scope"] = "request"
            if opts.get("conversation_id"):
                c["conversation_id"] = opts["conversation_id"]
        c["timeout"] = int(opts.get("timeout", 30))
        if opts.get("title"):
            c["title"] = opts["title"]
        if opts.get("subtitle"):
            c["subtitle"] = opts["subtitle"]
        if opts.get("message") and c.get("type") in ("message",):
            c["message"] = opts["message"]
        if (img := device.get("image")) and img[1]:
            if img[0] == "camera":
                token = next((t for t in entry["pictures"] if self.pictures.get(t, ("",))[0] == img[1]), None)
                if not token:
                    token = secrets.token_urlsafe(18)
                    entry["pictures"].append(token)
                self.pictures[token] = (img[1], entry["expires"])
                c["image"] = f"{self._base_url()}/api/hassmic_cards/camera/{token}"
            else:
                c["image"] = img[1] if re.match(r"https?://", img[1]) else self._base_url() + ("" if img[1].startswith("/") else "/") + img[1]
        if acts := opts.get("actions"):
            c["buttons"] = [{"label": a["title"], "id": a["action"], **({"keep": True} if a.get("keep") else {})} for a in acts[:3]]
        # what does not fit: rows off the end, then the text shorter (the Echo refuses a card over its size, never cuts it)
        size = lambda: len(json.dumps(c, ensure_ascii=False).encode())
        for rows in (c.get("items"), c.get("entities"), (c.get("weather") or {}).get("forecast")):
            while size() > CARD_JSON_MAX and isinstance(rows, list) and rows:
                rows.pop()
        for key in ("message", "subtitle", "title"):
            while size() > CARD_JSON_MAX and isinstance(c.get(key), str) and c[key]:
                c[key] = c[key][: max(0, len(c[key]) - max(64, size() - CARD_JSON_MAX))]
        return c

    async def _clear_on(self, echos: list[str], cid: str) -> None:
        for node in echos:
            try:
                await self.hass.services.async_call("esphome", f"{node}_clear_card", {"id": cid}, blocking=False)
            except Exception as err:                            # noqa: BLE001
                _LOGGER.warning("clear_card on %s: %s", node, err)

    async def clear(self, cid: str | None, echos: list[str] | None) -> None:
        if cid:
            entry = self.cards.get(cid)
            await self._clear_on(echos or (entry["echos"] if entry else []), cid)
            if entry and (not echos or set(entry["echos"]) <= set(echos)):
                self.forget(cid)
            elif entry:
                entry["echos"] = [e for e in entry["echos"] if e not in echos]
                self._save()
        else:
            await self._clear_on(echos or [], "")

    # ------------------------------------------------------------------ in from the Echos

    async def on_action(self, event: Event, node_of: Callable[[str], str | None]) -> None:
        """a press on an Echo the card was sent to: any ESPHome device may fire esphome.* events, so an Echo Show's only,
        and the card's own Echos for a card of ours; a button without keep runs once (taken before anything is awaited:
        two Shows at once)"""
        cid, action, device_id = event.data.get("card", ""), event.data.get("action", ""), event.data.get("device_id")
        entry = self.cards.get(cid)
        node = node_of(device_id) if device_id else None
        if node is None:
            _LOGGER.warning("a card button event from a device that is no Echo Show (%s): ignored", device_id)
            return
        if entry is not None and node not in entry["echos"]:
            _LOGGER.warning("button %s of card %s pressed on a device the card is not on (%s): ignored", action, cid, device_id)
            return
        if entry is None and time.monotonic() - self._answered.get(cid, -ANSWERED_S) < ANSWERED_S:
            _LOGGER.debug("button %s of card %s: the card was just answered (on another Show): ignored", action, cid)
            return
        if entry is not None and entry.get("snoozed_until"):
            _LOGGER.debug("button %s of card %s: the card is snoozed (a press on another Show): ignored", action, cid)
            return
        spec = next((a for a in (entry or {}).get("opts", {}).get("actions") or [] if a["action"] == action), None)
        snooze = spec.get("snooze") if spec else None
        if spec and not spec.get("keep") and snooze is None:
            self.forget(cid)
            now = time.monotonic()
            self._answered = {c: t for c, t in self._answered.items() if now - t < ANSWERED_S}
            self._answered[cid] = now
        elif snooze is not None:                                # off every Echo now, back after it (kept over a restart)
            entry["snoozed_until"] = time.time() + float(snooze) * 60
            for d in (self._unsub, self._pending):
                if (f := d.pop(cid, None)):
                    f()
            self._schedule_return(cid)
            self._save()
        self.hass.bus.async_fire(ACTION_EVENT, {"action": action, "card": cid, "device_id": device_id}, context=event.context)
        if spec is None:
            _LOGGER.debug("button %s of card %s: no card of ours (gone, or not hassmic_cards')", action, cid)
            return
        if spec.get("perform_action"):
            domain, service = spec["perform_action"].split(".", 1)
            try:                                                # (a renamed script: the card still goes from every Show)
                await self.hass.services.async_call(domain, service, dict(spec.get("data") or {}), blocking=False, context=event.context)
            except (HomeAssistantError, vol.Invalid) as err:
                _LOGGER.warning("button %s of card %s: %s failed: %s", action, cid, spec["perform_action"], err)
        if not spec.get("keep") or snooze is not None:          # done with (for now): off every Echo it was on
            await self._clear_on(entry["echos"], cid)
