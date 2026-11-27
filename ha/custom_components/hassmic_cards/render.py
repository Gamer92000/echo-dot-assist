"""Dashboard cards (the subset an Echo Show can draw) into the Echo's own card format (docs/cards.md, "The Echo's card").

Every word on the Echo's screen is made here, from Home Assistant's own translations where it has them (entity states:
"component.<domain>.entity_component[.<device_class>].state.<state>", "All day"), and language-neutral otherwise
(numbers, units, "20.5° → 21°", "↑68° ↓55°"). The Echo composes no text.

Supported dashboard cards: entities, glance, tile, weather-forecast, markdown, picture-entity, picture-glance, picture,
todo-list, calendar. A card of the library: the dashboard whose URL is the "library" option, one view per card (its URL
the card's name), the view's first card; built and previewed with Home Assistant's own card editor. Its strings may be
templates of the `variables` it is shown with ("{{ minutes }}"); a markdown card's content keeps its own template
(and with it, the entities it follows). Extensions (ignored by the dashboard): "secure_controls" on entities/tile (covers of a garage,
gate or door get controls), "focus" on an entities row, "days"/"offset" on calendar.
"""
from __future__ import annotations

import copy
import logging
import re
from datetime import timedelta
from typing import Any

from homeassistant.components.homeassistant.exposed_entities import async_should_expose
from homeassistant.core import HomeAssistant, State
from homeassistant.helpers import area_registry as ar
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.template import Template, render_complex
from homeassistant.helpers.translation import async_get_translations
from homeassistant.util import dt as dt_util

_LOGGER = logging.getLogger(__name__)

CARD_TYPES = ("entities", "glance", "tile", "weather-forecast", "markdown", "picture-entity", "picture-glance", "picture",
              "todo-list", "calendar")
TOGGLES = ("switch", "input_boolean", "fan", "automation", "humidifier", "siren")
NEVER = ("lock", "alarm_control_panel")                   # no control on a screen anyone walks past (the Echo agrees)
SECURE_COVERS = ("garage", "gate", "door")                # read-only unless the card says secure_controls
ROOM_DOMAINS = ("light", "switch", "fan", "cover", "climate", "media_player", "lock", "input_boolean")
ACTIVE_PLAYER = ("playing", "paused", "buffering", "on")  # a room's media players shown only while in use
MAX_ROWS, MAX_ITEMS, MAX_DAYS = 8, 12, 5
# countries that read the clock in 12 hours (Home Assistant's country setting; the rest: 24)
TWELVE_HOURS = {"US", "CA", "AU", "NZ", "PH", "IN", "PK", "BD", "EG", "SA", "MY", "CO", "MX"}
# weekdays as a forecast's column heads, by Home Assistant's language (Monday first); English for the rest
WEEKDAYS = {
    "en": "Mon Tue Wed Thu Fri Sat Sun", "de": "Mo Di Mi Do Fr Sa So", "fr": "lun. mar. mer. jeu. ven. sam. dim.",
    "es": "lun mar mié jue vie sáb dom", "it": "lun mar mer gio ven sab dom", "nl": "ma di wo do vr za zo",
    "pt": "seg ter qua qui sex sáb dom", "sv": "mån tis ons tors fre lör sön", "da": "man tir ons tor fre lør søn",
    "nb": "man tir ons tor fre lør søn", "fi": "ma ti ke to pe la su", "pl": "pon wt śr czw pt sob niedz",
    "cs": "po út st čt pá so ne",
}

class CardError(Exception):
    """a card that cannot be shown: its reason for the person who wrote it"""


class Render:
    def __init__(self, hass: HomeAssistant, library: str = "echo-cards") -> None:
        self.hass = hass
        self.library = library
        self._words: dict[str, str] = {}
        self._loaded: set[str] = set()

    # ------------------------------------------------------------------ words

    async def load(self, domains: set[str]) -> None:
        """Home Assistant's translations of these domains' states (and calendar's "All day"), once each"""
        new = (set(domains) | {"calendar"}) - self._loaded
        if not new:
            return
        lang = self.hass.config.language
        for cat in ("entity_component", "state", "title"):
            try:
                self._words.update(await async_get_translations(self.hass, lang, cat, list(new)))
            except Exception:                                   # noqa: BLE001  (words fall back to the state as written)
                pass
        self._loaded |= new

    def word(self, st: State) -> str:
        """a state as Home Assistant's frontend words it (its translation), else as written, readable"""
        domain, dc = st.domain, st.attributes.get("device_class")
        for key in ([f"component.{domain}.entity_component.{dc}.state.{st.state}"] if dc else []) + \
                   [f"component.{domain}.entity_component._.state.{st.state}", f"component.{domain}.state._.{st.state}"]:
            if key in self._words:
                return self._words[key]
        s = st.state.replace("_", " ")
        return s[:1].upper() + s[1:]

    def title(self, domain: str, default: str) -> str:
        """an integration's own name as Home Assistant words it ("Weather")"""
        return self._words.get(f"component.{domain}.title", default)

    def all_day(self) -> str:
        return self._words.get("component.calendar.entity_component._.state_attributes.all_day.name", "All day")

    def number(self, st: State) -> str | None:
        """a numeric state with its unit, at the entity's display precision (else at most one decimal)"""
        try:
            v = float(st.state)
        except (TypeError, ValueError):
            return None
        ent = er.async_get(self.hass).async_get(st.entity_id)
        prec = None
        if ent:
            o = ent.options.get("sensor") or {}
            prec = o.get("display_precision", o.get("suggested_display_precision"))
        if prec is None:
            dec = st.state.split(".", 1)[1] if "." in st.state else ""
            prec = min(len(dec), 1)
        txt = f"{v:.{int(prec)}f}"
        unit = st.attributes.get("unit_of_measurement")
        return f"{txt} {unit}" if unit else txt

    @staticmethod
    def deg(v: Any) -> str:
        return f"{round(v)}°" if isinstance(v, (int, float)) else ""

    def clock(self, t) -> str:
        """a time as people of Home Assistant's country read it (12 or 24 hours)"""
        if (self.hass.config.country or "").upper() in TWELVE_HOURS:
            return t.strftime("%-I:%M %p")
        return t.strftime("%H:%M")

    def weekday(self, d) -> str:
        lang = (self.hass.config.language or "en").split("-")[0].lower()
        return (WEEKDAYS.get(lang) or WEEKDAYS["en"]).split()[d.weekday()]

    # ------------------------------------------------------------------ rows

    def area_of(self, entity_id: str) -> str | None:
        ent = er.async_get(self.hass).async_get(entity_id)
        if not ent:
            return None
        if ent.area_id:
            return ent.area_id
        dev = dr.async_get(self.hass).async_get(ent.device_id) if ent.device_id else None
        return dev.area_id if dev else None

    def area_name(self, area_id: str | None) -> str | None:
        area = ar.async_get(self.hass).async_get_area(area_id) if area_id else None
        return area.name if area else None

    def row(self, entity_id: str, name: str | None = None, focus: bool = False, read_only: bool = False,
            area_name: str | None = None, secure: bool = False) -> dict | None:
        st = self.hass.states.get(entity_id)
        if st is None:
            return None
        name = str(name) if name not in (None, "") else ""    # (a template's result may be a number)
        if not name:
            name = st.name or entity_id
            if area_name and name.lower().startswith(area_name.lower() + " ") and len(name) > len(area_name) + 1:
                name = name[len(area_name) + 1:]                # "Kitchen Light" on the Kitchen's card: "Light"
                name = name[:1].upper() + name[1:]
        a, domain = st.attributes, st.domain
        row: dict[str, Any] = {"entity_id": entity_id, "name": name, "focus": focus}
        there = st.state not in ("unavailable", "unknown")
        word = self.word(st)
        ctl = "read"
        if domain == "light":
            modes = a.get("supported_color_modes") or []
            dimmable = bool(modes) and list(modes) != ["onoff"]
            pct = round(a["brightness"] * 100 / 255) if st.state == "on" and a.get("brightness") is not None else None
            row["value"] = f"{word} · {pct} %" if pct is not None else word
            ctl = "dim" if dimmable else "toggle"
            row["on"] = st.state == "on"
            if dimmable:
                row["level"] = pct if pct is not None else 0
        elif domain in TOGGLES:
            row["value"], ctl, row["on"] = word, "toggle", st.state == "on"
        elif domain == "cover":
            pos = a.get("current_position")
            row["value"] = f"{word} · {pos} %" if pos is not None and st.state != "closed" else word
            ctl = "cover" if secure or a.get("device_class") not in SECURE_COVERS else "read"
            row["on"] = st.state != "closed"
            if pos is not None:
                row["level"] = pos
        elif domain == "climate":
            u = "°"
            cur, sp = a.get("current_temperature"), a.get("temperature")
            if st.state == "off" or sp is None:
                row["value"] = word if cur is None else f"{word} · {cur}{u}"
            else:
                row["value"] = f"{cur}{u} → {sp}{u}" if cur is not None else f"→ {sp}{u}"
                step = a.get("target_temp_step") or (1 if "F" in self.hass.config.units.temperature_unit else 0.5)
                ctl, row["setpoint"], row["step"] = "climate", sp, step
        elif domain == "media_player":
            f = a.get("supported_features") or 0
            title = a.get("media_title")
            row["value"] = f"{word} · {title}" if title and st.state in ("playing", "paused") else word
            if a.get("device_class") in ("tv", "receiver") and f & 128 and f & 256:   # TURN_ON | TURN_OFF: its power
                ctl, row["on"] = "toggle", st.state not in ("off", "standby", "unavailable", "unknown")
            else:
                ctl, row["on"] = "play", st.state == "playing"
        elif domain in ("scene", "script", "button"):
            row["value"], ctl = "", "run"
        else:
            row["value"] = self.number(st) or word
        if domain in NEVER or read_only or not there:
            ctl = "read"
        row["control"] = ctl
        return row

    def room(self, area_id: str, skip: set[str]) -> list[str]:
        """the exposed, controllable entities of an area (directly or by their device), not already on the card"""
        entreg, devreg = er.async_get(self.hass), dr.async_get(self.hass)
        out = []
        for ent in entreg.entities.values():
            if ent.entity_id in skip or ent.disabled_by or ent.hidden_by or ent.domain not in ROOM_DOMAINS:
                continue
            area = ent.area_id or (devreg.async_get(ent.device_id).area_id if ent.device_id and devreg.async_get(ent.device_id) else None)
            if area != area_id or not async_should_expose(self.hass, "conversation", ent.entity_id):
                continue
            st = self.hass.states.get(ent.entity_id)
            if st is None or st.state == "unavailable":
                continue
            if ent.domain == "media_player" and st.state not in ACTIVE_PLAYER:
                continue                                        # an idle Echo or a TV that is off: not worth a row
            out.append(ent.entity_id)
        out.sort(key=lambda e: (ROOM_DOMAINS.index(e.split(".", 1)[0]), (self.hass.states.get(e).name or e).lower()))
        return out

    # ------------------------------------------------------------------ cards

    async def card(self, cfg: dict) -> tuple[dict, set[str]]:
        """a dashboard card into the Echo's card (without v, id, scope, timeout: the caller's), and the entities whose
        changes change it"""
        kind = cfg.get("type")
        variables: dict = {}
        if kind == "_library":                                  # one of the library's, by name
            variables = dict(cfg.get("variables") or {})
            cfg = await self.library_card(str(cfg.get("name", "")))
            kind = cfg.get("type")
        elif "_variables" in cfg:
            variables = dict(cfg["_variables"])
            cfg = {k: v for k, v in cfg.items() if k != "_variables"}
        if variables:
            cfg = self.with_variables(cfg, variables)
        if kind == "_text":                                     # words as they are (an answer, a notification): no template
            return {"type": "message", "title": str(cfg.get("title", "")), "message": str(cfg.get("text", ""))}, set()
        if kind not in CARD_TYPES:
            raise CardError(f"card type {kind!r}: an Echo Show draws {', '.join(CARD_TYPES)}")
        fn = getattr(self, "_" + kind.replace("-", "_"))
        if kind == "markdown" and variables:
            cfg = {**cfg, "_vars": variables}                   # (markdown renders its content with them)
        return await fn(cfg)

    async def library_card(self, name: str) -> dict:
        """the library dashboard's view of that URL (or title): its first card"""
        from homeassistant.components.lovelace.const import LOVELACE_DATA
        data = self.hass.data.get(LOVELACE_DATA)
        dash = data.dashboards.get(self.library) if data else None
        if dash is None:
            raise CardError(f"no card library: create a dashboard with the URL {self.library!r} (Settings → Dashboards), "
                            f"a view per card, the view's URL its name")
        try:
            conf = await dash.async_load(False)
        except Exception as err:                                # noqa: BLE001  (an empty, never saved dashboard)
            raise CardError(f"the card library {self.library!r} has no cards yet ({err})") from err
        for view in conf.get("views") or []:
            if name in (view.get("path"), view.get("title")):
                cards = view.get("cards") or [c for sec in view.get("sections") or [] for c in sec.get("cards") or []]
                cards = [c for c in cards if c.get("type") not in ("heading",)]
                if not cards:
                    raise CardError(f"card {name!r}: its view in {self.library!r} has no card")
                return copy.deepcopy(cards[0])
        raise CardError(f"no card {name!r} in the card library {self.library!r} (a view whose URL is {name!r})")

    def with_variables(self, cfg: dict, variables: dict) -> dict:
        """the card's strings rendered as templates of these variables (a markdown card's content later, by itself)"""
        content = cfg.get("content") if cfg.get("type") == "markdown" else None
        rest = {k: v for k, v in cfg.items() if not (k == "content" and content is not None)}
        try:
            out = render_complex(cv.template_complex(rest), variables)
        except Exception as err:                                # noqa: BLE001
            raise CardError(f"the card's templates: {err}") from err
        if content is not None:
            out["content"] = content
        return out

    def _items(self, cfg: dict, key: str = "entities") -> list[dict]:
        out = []
        for e in cfg.get(key) or []:
            if isinstance(e, str):
                out.append({"entity": e})
            elif isinstance(e, dict) and (e.get("entity") or e.get("entity_id")):
                out.append({**e, "entity": e.get("entity") or e.get("entity_id")})
        return out

    async def _entities(self, cfg: dict, read_only: bool = False, with_room: bool = False) -> tuple[dict, set[str]]:
        with_room = with_room or bool(cfg.get("_room"))         # (a voice request's: what it did and the rest of its room)
        items = self._items(cfg)
        ids = [i["entity"] for i in items if self.hass.states.get(i["entity"])]
        if not ids:
            raise CardError("entities: none of them exists")
        area_id = self.area_of(ids[0])
        area = self.area_name(area_id)
        if with_room and area_id:
            items += [{"entity": e} for e in self.room(area_id, set(ids))]
        # a group's members not beside the group (a light group lists its lights in "entity_id")
        members = {m for i in items if (s := self.hass.states.get(i["entity"])) for m in (s.attributes.get("entity_id") or [])
                   if isinstance(m, str) and m != i["entity"]}
        rows_max = max(1, min(MAX_ROWS, int(cfg.get("_rows") or MAX_ROWS)))   # (a voice card: the option's rows)
        items = [i for i in items if i["entity"] not in members or i.get("focus")][:rows_max]
        await self.load({i["entity"].split(".", 1)[0] for i in items})
        secure = bool(cfg.get("secure_controls"))
        rows = [r for i in items if (r := self.row(i["entity"], i.get("name"), bool(i.get("focus")),
                                                   read_only or i.get("type") in ("simple-entity", "attribute"), area, secure))]
        if not rows:
            raise CardError("entities: none of them exists")
        title = str(cfg.get("title") or area or (rows[0]["name"] if len(rows) == 1 else ""))
        return {"type": "entities", "title": title, "entities": rows}, {r["entity_id"] for r in rows}

    async def _glance(self, cfg: dict):
        return await self._entities(cfg, read_only=True)

    async def _tile(self, cfg: dict):
        e = cfg.get("entity")
        if not e:
            raise CardError("tile: no entity")
        return await self._entities({"title": cfg.get("title", ""), "secure_controls": cfg.get("secure_controls"),
                                     "entities": [{"entity": e, "name": cfg.get("name"), "focus": True}]})

    async def _weather_forecast(self, cfg: dict):
        entity_id = cfg.get("entity")
        st = self.hass.states.get(entity_id) if entity_id else None
        if st is None:
            raise CardError("weather-forecast: no such weather entity")
        a = st.attributes
        forecast: list[dict] = []
        kinds = [cfg["forecast_type"]] if cfg.get("forecast_type") in ("daily", "twice_daily") else ["daily", "twice_daily"]
        for kind in kinds + [k for k in ("daily", "twice_daily") if k not in kinds]:
            try:
                resp = await self.hass.services.async_call("weather", "get_forecasts", {"entity_id": entity_id, "type": kind},
                                                           blocking=True, return_response=True)
                forecast = (resp or {}).get(entity_id, {}).get("forecast") or []
            except Exception:                                   # noqa: BLE001
                forecast = []
            if forecast:
                break
        # by day: a daily forecast as it is; twice_daily (NWS: day and night periods) merged, a night with the day it
        # starts on, so a day has its high (the day's) and low (the night's)
        today = dt_util.now().date()
        merged: dict = {}
        for f in forecast:
            t = dt_util.parse_datetime(f.get("datetime", "")) if f.get("datetime") else None
            if t is None:
                continue
            d = dt_util.as_local(t).date()
            m = merged.setdefault(d, {"date": d, "hi": None, "lo": None, "cond": "", "pop": None})
            if f.get("is_daytime", True):
                m["hi"] = f.get("temperature")
                if f.get("templow") is not None:
                    m["lo"] = f["templow"]
                m["cond"] = f.get("condition") or m["cond"]
            else:
                m["lo"] = f.get("temperature") if f.get("templow") is None else f["templow"]
                m["cond"] = m["cond"] or f.get("condition") or ""
            p = f.get("precipitation_probability")
            if isinstance(p, (int, float)):
                m["pop"] = p if m["pop"] is None else max(m["pop"], p)
        days_ = sorted(merged.values(), key=lambda m: m["date"])
        days_ = [m for m in days_ if m["date"] >= today] or days_
        detail = ""
        if days_:
            m = days_[0]
            detail = "  ".join(x for x in (f"↑{self.deg(m['hi'])}" if m["hi"] is not None else "",
                                            f"↓{self.deg(m['lo'])}" if m["lo"] is not None else "") if x)
        days = []
        if cfg.get("show_forecast", True):
            for m in days_[1:1 + MAX_DAYS]:                     # the days after today (today's is in the header)
                hi, lo = m["hi"], m["lo"]
                temps = f"{self.deg(hi)} / {self.deg(lo)}" if hi is not None and lo is not None else self.deg(hi if hi is not None else lo)
                days.append({"label": self.weekday(m["date"]), "condition": m["cond"], "temps": temps,
                             "precip": f"{round(m['pop'])} %" if m["pop"] is not None else ""})
        await self.load({"weather"})
        title = cfg["name"] if cfg.get("name") is not None else (cfg.get("title") or st.name or "")
        return ({"type": "weather", "title": title or self.title("weather", "Weather"),
                 "weather": {"condition": st.state if st.state not in ("unknown", "unavailable") else "",
                             "temperature": self.deg(a.get("temperature")), "detail": detail, "forecast": days}}, {entity_id})

    async def _markdown(self, cfg: dict):
        content = str(cfg.get("content", ""))
        tpl = Template(content, self.hass)
        info = tpl.async_render_to_info(cfg.get("_vars") or None)   # (once: its words and the entities it read)
        if info.exception:
            raise CardError(f"markdown: {info.exception}")
        return {"type": "message", "title": str(cfg.get("title", "")), "message": plain(str(info.result()))}, set(info.entities)

    async def _picture_entity(self, cfg: dict, picture=None):
        entity_id = cfg.get("entity") or cfg.get("camera_image")
        st = self.hass.states.get(entity_id) if entity_id else None
        if st is None:
            raise CardError(f"{cfg.get('type')}: no such entity")
        card = {"type": "message", "title": cfg.get("name") or cfg.get("title") or (st.name if cfg.get("show_name", True) else ""),
                "image": ("camera", entity_id) if st.domain == "camera" else ("url", st.attributes.get("entity_picture", ""))}
        if st.domain == "camera" and cfg.get("camera_view") == "live":
            card["image_refresh"] = int(cfg.get("refresh", 2))
        elif cfg.get("refresh"):
            card["image_refresh"] = int(cfg["refresh"])
        if st.domain != "camera" and cfg.get("show_state", True):
            await self.load({st.domain})
            card["message"] = self.number(st) or self.word(st)
        return card, ({entity_id} if st.domain != "camera" else set())

    async def _picture_glance(self, cfg: dict):
        card, ents = await self._picture_entity({**cfg, "entity": cfg.get("camera_image") or cfg.get("entity"), "show_state": False}) \
            if (cfg.get("camera_image") or cfg.get("entity")) else ({"type": "message", "title": cfg.get("title", ""), "image": ("url", cfg.get("image", ""))}, set())
        items = self._items(cfg)
        await self.load({i["entity"].split(".", 1)[0] for i in items})
        bits = []
        for i in items[:6]:
            st = self.hass.states.get(i["entity"])
            if st:
                bits.append(f"{i.get('name') or st.name}: {self.number(st) or self.word(st)}")
                ents.add(i["entity"])
        if bits:
            card["message"] = " · ".join(bits)
        card["title"] = cfg.get("title") or card.get("title", "")
        return card, ents

    async def _picture(self, cfg: dict):
        if not cfg.get("image"):
            raise CardError("picture: no image")
        return {"type": "message", "title": cfg.get("title", ""), "image": ("url", cfg["image"])}, set()

    async def _todo_list(self, cfg: dict):
        entity_id = cfg.get("entity")
        st = self.hass.states.get(entity_id) if entity_id else None
        if st is None:
            raise CardError("todo-list: no such to-do list")
        try:
            resp = await self.hass.services.async_call("todo", "get_items", {"entity_id": entity_id, "status": "needs_action"},
                                                       blocking=True, return_response=True)
            items = (resp or {}).get(entity_id, {}).get("items") or []
        except Exception as err:                                # noqa: BLE001
            raise CardError(f"todo-list: {err}") from err
        rows = [{"text": cap(i.get("summary", "")), "detail": i.get("description") or ""} for i in items if i.get("summary")]
        return {"type": "list", "title": cfg.get("title") or st.name, "items": rows[:MAX_ITEMS], "count": len(rows)}, {entity_id}

    async def _calendar(self, cfg: dict):
        cals = [e if isinstance(e, str) else e.get("entity") for e in cfg.get("entities") or []]
        cals = [c for c in cals if c and self.hass.states.get(c) and self.hass.states.get(c).state not in ("unavailable", "unknown")]
        if not cals:
            raise CardError("calendar: none of its calendars is there")
        start = dt_util.start_of_local_day() + timedelta(days=int(cfg.get("offset", 0)))
        end = start + timedelta(days=max(1, int(cfg.get("days", 1))))
        try:
            resp = await self.hass.services.async_call("calendar", "get_events",
                                                       {"entity_id": cals, "start_date_time": start.isoformat(), "end_date_time": end.isoformat()},
                                                       blocking=True, return_response=True)
        except Exception as err:                                # noqa: BLE001
            raise CardError(f"calendar: {err}") from err
        await self.load({"calendar"})
        seen, events = set(), []
        for cal, r in (resp or {}).items():
            name = self.hass.states.get(cal).name if self.hass.states.get(cal) else cal
            for e in (r or {}).get("events") or []:
                key = (e.get("summary"), e.get("start"))
                if key in seen:
                    continue
                seen.add(key)
                allday = "T" not in str(e.get("start", ""))
                if allday:
                    when = self.all_day()
                else:
                    s = dt_util.as_local(dt_util.parse_datetime(e["start"]))
                    f = dt_util.parse_datetime(e.get("end", "")) if e.get("end") else None
                    when = self.clock(s) + (f" – {self.clock(dt_util.as_local(f))}" if f else "")
                events.append((str(e.get("start")), {"text": e.get("summary", ""), "detail": f"{when} · {name}"}))
        events.sort(key=lambda x: x[0])
        items = [e for _, e in events][:MAX_ITEMS]
        title = cfg.get("title") or f"{self.weekday(start)} {start.day}"
        return {"type": "list", "title": title, "items": items, "count": len(events)}, set(cals)


def plain(md: str) -> str:
    """markdown as plain text for the Echo's message (one paragraph: the card has no markup)"""
    t = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", md)                # images
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", t)             # links: their words
    t = re.sub(r"<[^>]+>", "", t)                               # html
    t = re.sub(r"^\s{0,3}(#{1,6}|[-*+]|\d+\.)\s+", "", t, flags=re.M)
    t = re.sub(r"(\*\*|__|\*|_|`|~~)", "", t)
    return re.sub(r"\s+", " ", t).strip()


def cap(s: str) -> str:
    return s[:1].upper() + s[1:]
