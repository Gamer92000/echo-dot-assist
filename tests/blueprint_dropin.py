#!/usr/bin/env python3
"""The Drop In voice blueprint (blueprints/automation/drop_in.yaml) run against stand-ins of Home Assistant: its
registries (devices, entities, areas, floors), states and actions, and its template environment (jinja2's sandbox with
HA's extensions, python-slugify as HA's slugify, HA's result parser), with a small runner for the blueprint's actions
(if, choose, repeat, variables, wait_template, set_conversation_response, stop, actions incl. automation.trigger and
ServiceNotFound).  Each function below is worded after Home Assistant 2026.10's (helpers/template/extensions,
helpers/template/helpers.py resolve_area_id, helpers/script.py), as far as the blueprint uses them; the blueprint was
also run in a real Home Assistant against three build/hassmic-host, one of them a released build (PLAN.md).

Cases: renamed entity ids and devices, umlauts, two Echos in one area, Drop In off on either side, other ESPHome
devices with look-alike entities, manufacturer "Amazon" (HA without a project name), an action under
the second name only or under none, busy and offline Echos, floors, aliases, hand-set replies with two entries.
Echos as released before 2026-10-11 (only "Drop In") and after (also "Drop In result", and "calling" before any
refusal), each as caller and as callee: every result's reply, and how long Home Assistant would wait.

Also: every devices/*/board.c project's second half (Home Assistant's model) is the slug of its default name, which the
blueprint's second action name relies on."""
import ast, glob, os, re, sys
import jinja2, yaml
from jinja2.sandbox import ImmutableSandboxedEnvironment
from slugify import slugify as _slugify

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLUEPRINT = os.path.join(ROOT, "blueprints", "automation", "drop_in.yaml")


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


# ------------------------------------------------------------------ Home Assistant stand-ins

def ha_slugify(text, separator="_"):                     # homeassistant/util/__init__.py slugify
    if text == "" or text is None: return ""
    slug = _slugify(text, separator=separator)
    return "unknown" if slug == "" else slug


def normalize_name(name): return name.casefold().replace(" ", "")      # normalized_name_base_registry.py


class HA:
    """registries and states, as plain dicts"""
    def __init__(self):
        self.areas, self.floors, self.devices, self.entities, self.states = {}, {}, {}, {}, {}
        self.services, self.calls, self.entries = {}, [], {}
        self.pending = []                               # state changes on their way to Home Assistant, in order
        self.events = 0                                 # event entities' times
        self.waited = 0                                 # seconds the waits that ran out took (their timeouts)

    def area(self, aid, name, floor=None, aliases=()): self.areas[aid] = dict(name=name, floor=floor, aliases=list(aliases))
    def floor(self, fid, name, aliases=()): self.floors[fid] = dict(name=name, aliases=list(aliases))

    def device(self, did, name, manufacturer, model, mac=None, area=None, name_by_user=None):
        self.devices[did] = dict(name=name, manufacturer=manufacturer, model=model, area_id=area, name_by_user=name_by_user,
                                 connections={("mac", mac.lower())} if mac else set())

    def entity(self, eid, device, state=None, platform="esphome", area=None, icon=None, disabled=False, attrs=None):
        self.entities[eid] = dict(device_id=device, platform=platform, area_id=area, disabled=disabled)
        if state is not None and not disabled:
            self.states[eid] = dict(state=state, attributes=dict({"icon": icon} if icon else {}, **(attrs or {})))

    def set_state(self, eid, state): self.states[eid]["state"] = state

    def deliver(self):
        """the next state change; False when none is left (a wait times out)"""
        if not self.pending: return False
        e, st = self.pending.pop(0)
        if e in self.states and isinstance(st, tuple):  # an event: a new time (each later than the last), its type
            self.events += 1
            self.states[e]["state"] = f"2026-10-11T12:{self.events // 60:02d}:{self.events % 60:02d}.000+00:00"
            self.states[e]["attributes"]["event_type"] = st[1]
        elif e in self.states: self.states[e]["state"] = st
        return True

    # helpers/template/helpers.py resolve_area_id
    def resolve_area_id(self, v):
        v = str(v)
        for aid, a in self.areas.items():
            if normalize_name(a["name"]) == normalize_name(v): return aid
        for aid, a in self.areas.items():
            if any(normalize_name(x) == normalize_name(v) for x in a["aliases"]): return aid
        if re.match(r"^\w+\.\w+$", v) and v in self.entities:
            e = self.entities[v]
            if e["area_id"]: return e["area_id"]
            if e["device_id"] in self.devices: return self.devices[e["device_id"]]["area_id"]
            return None
        if v in self.devices: return self.devices[v]["area_id"]
        return None


class States:
    """`states` in templates: states('x'), states.domain"""
    class S:
        def __init__(self, eid, st): self.entity_id, self.state, self.attributes = eid, st["state"], st["attributes"]
    def __init__(self, ha): self.ha = ha
    def __call__(self, eid):
        st = self.ha.states.get(eid)
        return st["state"] if st else "unknown"
    def __getattr__(self, domain):
        return [self.S(e, s) for e, s in sorted(self.ha.states.items()) if e.startswith(domain + ".")]


def environment(ha):
    env = ImmutableSandboxedEnvironment(undefined=jinja2.Undefined, extensions=["jinja2.ext.loopcontrols", "jinja2.ext.do"])
    def integration_entities(name):
        ents = [e for e in ha.entities for t, ids in ha.entries.items() if t == name and e in ids]
        if ents: return ents                            # a config entry titled so: only its entities
        return [e for e, x in ha.entities.items() if x["platform"] == name and not x["disabled"]]
    def device_entities(d): return [e for e, x in ha.entities.items() if x["device_id"] == d]
    def device_id(v):
        if v in ha.entities: return ha.entities[v]["device_id"]
        return next((d for d, x in ha.devices.items() if (x["name_by_user"] or x["name"]) == str(v)), None)
    def device_attr(v, attr):
        d = ha.devices.get(device_id(v) if "." in v else v)
        if d is None: return None
        return d.get(attr) if attr in d or attr in ("model_id", "hw_version") else None
    def area_name(v):
        if v in ha.areas: return ha.areas[v]["name"]
        if re.match(r"^\w+\.\w+$", str(v)) and v in ha.entities:
            e = ha.entities[v]
            if e["area_id"]: return ha.areas[e["area_id"]]["name"]
            if e["device_id"] in ha.devices and ha.devices[e["device_id"]]["area_id"]:
                return ha.areas[ha.devices[e["device_id"]]["area_id"]]["name"]
        if v in ha.devices and ha.devices[v]["area_id"]: return ha.areas[ha.devices[v]["area_id"]]["name"]
        return None
    def floor_id(v):
        for fid, f in ha.floors.items():
            if normalize_name(f["name"]) == normalize_name(str(v)): return fid
        for fid, f in ha.floors.items():
            if any(normalize_name(x) == normalize_name(str(v)) for x in f["aliases"]): return fid
        aid = ha.resolve_area_id(v)
        return ha.areas[aid]["floor"] if aid else None
    def floor_name(v):
        if v in ha.floors: return ha.floors[v]["name"]
        aid = ha.resolve_area_id(v)
        f = ha.areas[aid]["floor"] if aid else None
        return ha.floors[f]["name"] if f else None
    def state_attr(e, a): return ha.states[e]["attributes"].get(a) if e in ha.states else None
    def is_state(e, s):
        st = ha.states.get(e)
        return st is not None and (st["state"] == s or (isinstance(s, list) and st["state"] in s))
    def regex_match(v, find="", ignorecase=False): return bool(re.match(find, str(v), re.I if ignorecase else 0))
    def regex_search(v, find="", ignorecase=False): return bool(re.search(find, str(v), re.I if ignorecase else 0))
    def regex_replace(v="", find="", replace="", ignorecase=False):
        return re.sub(find, replace, str(v), flags=re.I if ignorecase else 0)
    def is_device_attr(d, a, v): return device_attr(d, a) == v
    states = States(ha)
    for n, f in dict(integration_entities=integration_entities, device_entities=device_entities, device_id=device_id,
                     device_attr=device_attr, area_id=ha.resolve_area_id, area_name=area_name, floor_id=floor_id,
                     floor_name=floor_name, states=states, state_attr=state_attr, is_state=is_state, slugify=ha_slugify,
                     is_device_attr=is_device_attr).items():
        env.globals[n] = f
    for n, f in dict(device_entities=device_entities, device_id=device_id, device_attr=device_attr, area_id=ha.resolve_area_id,
                     area_name=area_name, floor_id=floor_id, floor_name=floor_name, state_attr=state_attr, slugify=ha_slugify,
                     regex_replace=regex_replace, regex_match=regex_match, regex_search=regex_search, states=states).items():
        env.filters[n] = f
    for n, f in dict(match=regex_match, search=regex_search, is_state=is_state, is_device_attr=is_device_attr).items():
        env.tests[n] = f
    return env


_IS_NUMERIC = re.compile(r"^[+-]?(?!0\d)\d*(?:\.\d*)?$")

def parse_result(r):                                     # helpers/template/__init__.py _parse_result
    r = r.strip()
    try: v = ast.literal_eval(r)
    except (ValueError, TypeError, SyntaxError, MemoryError): return r
    if not isinstance(v, (str, complex)) and (not isinstance(v, (int, float)) or isinstance(v, bool) or _IS_NUMERIC.match(r)):
        return v
    return r


# Each run has an environment of its own (its globals read that run's registries), but they are all made alike, so a
# template compiles to the same code in each: compiled once, not once per run (that was most of this test's 7 s).
_CODE = {}
def template(env, source):                               # env.from_string, with the compiled code shared
    if source not in _CODE: _CODE[source] = env.compile(source)
    return env.template_class.from_code(env, _CODE[source], env.make_globals(None), None)


def render(env, value, variables):                       # render_complex
    if isinstance(value, list): return [render(env, x, variables) for x in value]
    if isinstance(value, dict): return {render(env, k, variables): render(env, x, variables) for k, x in value.items()}
    if isinstance(value, str) and ("{{" in value or "{%" in value):
        return parse_result(template(env, value).render(**variables))
    return value


# ------------------------------------------------------------------ the automation

class ServiceNotFound(Exception): pass
class Stop(Exception): pass


class Automation:
    def __init__(self, ha, inputs=None, this=True):
        class L(yaml.SafeLoader): pass
        L.add_constructor("!input", lambda l, n: ("!input", l.construct_scalar(n)))
        bp = yaml.load(open(BLUEPRINT, encoding="utf-8"), Loader=L)
        defaults = {k: v.get("default") for k, v in bp["blueprint"]["input"].items()}
        defaults.update(inputs or {})
        sub = lambda v: (defaults[v[1]] if isinstance(v, tuple) and v[:1] == ("!input",) else
                         [sub(x) for x in v] if isinstance(v, list) else {k: sub(x) for k, x in v.items()} if isinstance(v, dict) else v)
        self.bp, self.ha, self.env = sub({k: v for k, v in bp.items() if k != "blueprint"}), ha, environment(ha)
        self.this = {"entity_id": "automation.echo_drop_in", "state": "on"} if this else None
        self.runs, self.errors = 0, []

    def run(self, trigger, extra=None):
        """automation async_trigger: variables, then the actions; what an action raises ends this run (logged)"""
        self.runs += 1
        v = {"this": self.this, "trigger": trigger, **(extra or {})}
        for k, x in self.bp["variables"].items():
            if k not in v: v[k] = render(self.env, x, v)
        self.vars, run = v, {"response": None}
        try: self.seq(self.bp["actions"], v, run)
        except Stop: pass
        except ServiceNotFound as e: self.errors.append(f"service not found: {e}"); return None
        except Exception as e: self.errors.append(f"error: {e}"); return None      # logged, the run ends
        return run["response"]

    def cond(self, c, v):
        if isinstance(c, str): return bool(render(self.env, c, v))
        if isinstance(c, list): return all(self.cond(x, v) for x in c)
        raise NotImplementedError(c)

    def seq(self, steps, v, run):
        v = dict(v)                                     # a nested sequence's own scope
        for s in steps:
            if "if" in s:
                self.seq(s["then"] if self.cond(s["if"], v) else s.get("else", []), v, run)
            elif "choose" in s:
                for o in s["choose"]:
                    if self.cond(o["conditions"], v): self.seq(o["sequence"], v, run); break
                else: self.seq(s.get("default", []), v, run)
            elif "repeat" in s:
                r = s["repeat"]; i = 0
                while True:
                    i += 1
                    w = dict(v, repeat={"index": i, "first": i == 1})
                    w = self.seq(r["sequence"], w, run)
                    if "until" in r and self.cond(r["until"], w): break
                    if i > 10: raise RuntimeError("repeat runs away")
            elif "variables" in s:
                for k, x in s["variables"].items(): v[k] = render(self.env, x, v)
            elif "set_conversation_response" in s:
                run["response"] = render(self.env, s["set_conversation_response"], v)
            elif "stop" in s:
                raise Stop()
            elif "wait_template" in s:                  # true now, or at one of the state changes still coming
                done = bool(render(self.env, s["wait_template"], v))
                while not done and self.ha.deliver(): done = bool(render(self.env, s["wait_template"], v))
                v["wait"] = {"completed": done, "remaining": 0}
                if not done: self.ha.waited += float(render(self.env, s.get("timeout", 0), v))
            elif "action" in s:
                try: self.action(render(self.env, s["action"], v), render(self.env, s.get("data", {}), v),
                                 render(self.env, s.get("target", {}), v))
                except ServiceNotFound:
                    raise                               # helpers/script.py: not caught by continue_on_error
                except Exception as e:
                    if not s.get("continue_on_error"): raise
                    self.errors.append(str(e))
            else:
                raise NotImplementedError(s)
        return v

    def action(self, name, data, target):
        self.ha.calls.append((name, data))
        if name == "automation.trigger":                # components/automation: runs it, catches what it raises
            assert target["entity_id"] == self.this["entity_id"], target
            self.run({"platform": None}, data.get("variables"))
            return
        if name not in self.ha.services: raise ServiceNotFound(name)
        self.ha.services[name](data)


# ------------------------------------------------------------------ a house of Echos

REFUSALS = {"dnd": "do_not_disturb", "muted": "microphones_off", "off": "drop_in_off", "busy": "busy"}


class Echo:
    """a hassmic Echo as Home Assistant has it: device, entities (renamable ids), the drop_in action; calls between
    Echos as dropin.c makes them (the target by name or node, same network, not busy).  new: as from 2026-10-11, with
    "Drop In result" and "calling" before the invite goes out"""
    def __init__(self, ha, did, model, default_name, mac, area=None, dropin=True, network="home", sensor_id=None,
                 manufacturer="hassmic", name_by_user=None, online=True, ids=None, new=False, result_id=None):
        self.ha, self.id, self.network = ha, did, network
        self.name = f"{default_name} {mac.replace(':', '')[-6:].lower()}"
        self.node = re.sub(r"[^a-z0-9]+", "-", self.name.lower()).strip("-")
        ha.device(did, self.name, manufacturer, model, mac, area, name_by_user)
        pre = ids or self.node.replace("-", "_")
        st = (lambda s: s if online else "unavailable")
        ha.entity(f"assist_satellite.{pre}_assist_satellite", did, st("idle"))
        ha.entity(f"media_player.{pre}_media_player", did, st("idle"))
        ha.entity(f"sensor.{pre}_ringing_timers", did, st(""), icon="mdi:timer-outline")
        self.sensor = None
        if dropin:
            self.sensor = sensor_id or f"sensor.{pre}_drop_in"
            ha.entity(self.sensor, did, st("idle"), icon="mdi:phone-in-talk")
            ha.entity(f"sensor.{pre}_drop_in_with", did, st(""), icon="mdi:account-voice")
            ha.entity(f"button.{pre}_end_drop_in", did, st("unknown"), icon="mdi:phone-hangup")
            ha.services[f"esphome.{self.node.replace('-', '_')}_drop_in"] = self.drop_in
            if new:
                self.result = result_id or f"event.{pre}_drop_in_result"
                ha.entity(self.result, did, st("2026-10-10T08:00:00.000+00:00"), icon="mdi:phone-check",
                          attrs={"event_types": ["connected", "ringing", "busy", "do_not_disturb", "microphones_off", "drop_in_off",
                                                 "declined", "not_answered", "no_answer", "not_found", "busy_here",
                                                 "microphones_off_here", "cancelled", "failed"], "event_type": "connected"})
        self.online, self.refuse, self.ask, self.new = online, False, False, new
        self.muted = self.busy = False                  # what this Echo knows and Home Assistant may not yet

    def state(self): return self.ha.states[self.sensor]["state"] if self.sensor in self.ha.states else None

    def drop_in(self, data):
        """dropin.c dropin_call, and what follows as state changes Home Assistant gets one after the other"""
        if not self.online: raise RuntimeError("Error communicating with device")     # HomeAssistantError: continuable
        node = re.sub(r"[^a-z0-9]+", "-", str(data["target"]).lower()).strip("-")
        peer = next((e for e in ECHOS if e.node == node and e is not self and e.network == self.network), None)
        if self.new: return self.drop_in_new(peer)
        if peer is None or self.state() != "idle": return                              # refused here: stays idle
        ev = self.ha.pending
        ev.append((self.sensor, "calling"))
        if peer.refuse == "fast":
            ev.pop()                                                                   # refused before hassmic's loop
        elif not peer.online or not peer.sensor or peer.refuse or peer.state() != "idle":  # reported "calling"
            ev.append((self.sensor, "idle"))                                           # refused, or no answer in 4 s
        elif peer.ask:
            ev.append((peer.sensor, "ringing"))                                        # the caller stays at calling
        else:
            ev += [(peer.sensor, "connected"), (self.sensor, "connected")]

    def drop_in_new(self, peer):
        """dropin.c from 2026-10-11: a result for every call, "calling" first whatever the other Echo says"""
        ev, res = self.ha.pending, lambda r: (self.result, ("event", r))
        if peer is None: ev.append(res("not_found")); return
        if self.muted: ev.append(res("microphones_off_here")); return
        if self.busy or self.state() != "idle": ev.append(res("busy_here")); return
        ev.append((self.sensor, "calling"))
        why = "drop_in_off" if not peer.sensor else REFUSALS.get(peer.refuse) or ("busy" if peer.state() != "idle" else None)
        if not peer.online: ev += [(self.sensor, "idle"), res("no_answer")]                  # 4 s later
        elif why: ev += [(self.sensor, "idle"), res(why)]
        elif peer.ask: ev += [(peer.sensor, "ringing"), res("ringing")]
        else: ev += [(peer.sensor, "connected"), (self.sensor, "connected"), res("connected")]


ECHOS = []


def house(new=False):
    ha = HA()
    ha.floor("ground", "Ground floor", aliases=["downstairs"]); ha.floor("first", "Upstairs")
    ha.area("office", "Office", "first"); ha.area("kitchen", "Küche", "ground"); ha.area("living", "Living Room", "ground", ["Lounge"])
    ha.area("bedroom", "Bedroom", "first"); ha.area("garage", "Garage")
    ECHOS[:] = [
        Echo(ha, "dev_office", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:1C:DF:2E", "office", new=new, result_id="event.buero_ergebnis"),
        Echo(ha, "dev_kitchen", "echo-dot-3", "Echo Dot 3", "AA:BB:CC:56:95:C4", "kitchen", sensor_id="sensor.kuchen_dropin_renamed", new=new),
        Echo(ha, "dev_living", "echo-2", "Echo 2", "AA:BB:CC:00:00:01", "living", name_by_user="Wohnzimmer-Echo", new=new),
    ]
    # not Echos: a Voice PE and an ESPHome gadget with look-alike entities
    ha.device("dev_vpe", "Home Assistant Voice 0a1b2c", "Nabu Casa", "Home Assistant Voice PE", "AA:BB:CC:0A:1B:2C", "office")
    ha.entity("assist_satellite.home_assistant_voice_0a1b2c_assist_satellite", "dev_vpe", "idle")
    ha.device("dev_gadget", "Garage door", "espressif", "esp32dev", "AA:BB:CC:11:22:33", "garage")
    ha.entity("sensor.garage_door_drop_in", "dev_gadget", "idle")
    ha.entity("sensor.garage_door_status", "dev_gadget", "idle")
    return ha


def ask(ha, room, source, lang="en", inputs=None, this=True, slots=True):
    a = Automation(ha, inputs, this)
    trig = {"platform": "conversation", "sentence": f"drop in {room}", "device_id": source, "user_input": {"language": lang}}
    if slots: trig["slots"] = {"room": room}
    ha.calls.clear()
    r = a.run(trig)
    while ha.deliver(): pass
    return r if r is not None else "Done", a


def drop_actions(ha): return [c for c in ha.calls if c[0].startswith("esphome.")]


def main():
    # 1. the plain call: by area, umlaut and case as said, the action named from the reported name, target its name
    ha = house(); off, kit, liv = ECHOS
    r, a = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche", f"'drop in küche' from the office: {r!r}")
    check(drop_actions(ha) == [("esphome.echo_dot_2_1cdf2e_drop_in", {"target": "Echo Dot 3 5695c4"})], f"  the office's action, kitchen by name: {drop_actions(ha)}")
    check(a.runs == 2 and not a.errors, f"  through one call run of its own ({a.runs} runs, errors {a.errors})")
    check(off.state() == "connected" and kit.state() == "connected", "  both Echos connected")
    for said in ("Kueche", "kuche", "KÜCHE.", "„Küche“", "the Küche"):
        ha = house(); r, _ = ask(ha, said, "dev_office")
        check(r == "Calling Küche", f"'drop in {said}': {r!r}")

    # 2. renamed device (name_by_user) and entity ids (the kitchen's Drop In sensor above already is): by area, by
    #    the name people gave it, by a word of it, by alias, by floor
    ha = house(); r, _ = ask(ha, "living room", "dev_office")
    check(r == "Calling Living Room" and drop_actions(ha)[0][1] == {"target": "Echo 2 000001"}, f"'drop in living room': {r!r} {drop_actions(ha)}")
    ha = house(); r, _ = ask(ha, "wohnzimmer echo", "dev_office")
    check(r == "Calling Wohnzimmer-Echo", f"'drop in wohnzimmer echo' (its name in HA): {r!r}")
    ha = house(); r, _ = ask(ha, "wohnzimmer", "dev_office")
    check(r == "Calling Wohnzimmer-Echo", f"'drop in wohnzimmer' (a word of its name): {r!r}")
    ha = house(); r, _ = ask(ha, "lounge", "dev_office")
    check(r == "Calling Living Room", f"'drop in lounge' (area alias): {r!r}")
    ha = house(); r, _ = ask(ha, "kitchen please", "dev_office")
    check(r == "There is no Echo in kitchen please", f"'kitchen please' with the area called Küche: {r!r}")
    ha = house(); r, _ = ask(ha, "living room please", "dev_office")
    check(r == "Calling Wohnzimmer-Echo" or r == "Calling Living Room", f"'living room please' (the area a word of it): {r!r}")
    ha = house(); r, _ = ask(ha, "5695c4", "dev_office")
    check(r == "Calling Echo Dot 3 5695c4", f"'drop in 5695c4' (a word of its reported name): {r!r}")
    ha = house(); r, _ = ask(ha, "ground floor", "dev_office")
    check(r.startswith("Calling ") and drop_actions(ha), f"'drop in ground floor' (two Echos there): {r!r}")
    ha = house(); r, _ = ask(ha, "downstairs", "dev_office")
    check(r == "Calling Ground floor", f"'drop in downstairs' (floor alias): {r!r}")

    # 3. the Echo spoken to, Drop In off on either side, not an Echo
    ha = house(); r, _ = ask(ha, "office", "dev_office")
    check(r == "This is the Echo in Office" and not drop_actions(ha), f"'drop in office' in the office: {r!r}")
    ha = house(); r, _ = ask(ha, "upstairs", "dev_office")
    check(r == "This is the Echo in Upstairs", f"'drop in upstairs' with only this Echo there: {r!r}")
    ha = house(); r, _ = ask(ha, "kitchen", "dev_vpe")
    check(r == "Only an Echo with Drop In on can drop in", f"said to a Voice PE: {r!r}")
    ha = house(); r, _ = ask(ha, "küche", "")
    check(r == "Only an Echo with Drop In on can drop in", f"typed in HA (no device): {r!r}")
    ha = house(); r, _ = ask(ha, "garage", "dev_office")
    check(r == "There is no Echo in garage", f"'drop in garage' (an ESPHome gadget with a sensor.*_drop_in, state idle): {r!r}")
    ha = house(); r, _ = ask(ha, "nowhere", "dev_office")
    check(r == "There is no Echo in nowhere", f"'drop in nowhere': {r!r}")
    ha = house(); ECHOS.append(Echo(ha, "dev_bed", "echo-dot-3", "Echo Dot 3", "AA:BB:CC:BE:D0:01", "bedroom", dropin=False))
    r, _ = ask(ha, "bedroom", "dev_office")
    check(r == "Drop In is off on Bedroom", f"'drop in bedroom' (Drop In off there): {r!r}")
    ha = house(); ECHOS.append(Echo(ha, "dev_bed", "echo-dot-3", "Echo Dot 3", "AA:BB:CC:BE:D0:01", "office", dropin=False))
    r, _ = ask(ha, "küche", "dev_bed")
    check(r == "Only an Echo with Drop In on can drop in", f"said to an Echo with Drop In off: {r!r}")

    # 4. two Echos in one area: the idle one with Drop In
    ha = house(); ECHOS.append(Echo(ha, "dev_k2", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:00:00:02", "kitchen", dropin=False))
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and drop_actions(ha)[0][1]["target"] == "Echo Dot 3 5695c4", f"two in the kitchen, one with Drop In off: {drop_actions(ha)}")
    ha = house(); ECHOS.append(Echo(ha, "dev_k2", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:00:00:02", "kitchen"))
    ha.set_state(ECHOS[1].sensor, "connected")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and drop_actions(ha)[0][1]["target"] == "Echo Dot 2 000002", f"two in the kitchen, one in a call: {drop_actions(ha)}")

    # 5. busy, offline, outside the network
    ha = house(); ha.set_state(ECHOS[1].sensor, "connected")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Küche is in a call" and not drop_actions(ha), f"the kitchen in a call: {r!r}")
    ha = house(); ha.set_state(ECHOS[0].sensor, "ringing")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "This Echo is in a call already", f"the office ringing: {r!r}")
    ha = house(); ECHOS.append(Echo(ha, "dev_g", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:00:00:03", "garage", online=False))
    r, a = ask(ha, "garage", "dev_office")
    check(r == "Garage is offline" and len(drop_actions(ha)) == 1, f"the garage Echo offline: {r!r}")
    ha = house(); ECHOS.append(Echo(ha, "dev_g", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:00:00:03", "garage", online=False,
                                    sensor_id="sensor.garage_intercom"))
    r, a = ask(ha, "garage", "dev_office")
    check(r == "Garage is offline", f"  with its Drop In sensor renamed too (found by its icon): {r!r}")
    ha = house(); ECHOS.append(Echo(ha, "dev_g", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:00:00:03", "garage", network="other"))
    r, _ = ask(ha, "garage", "dev_office")
    check(r == "The call to Garage did not go through", f"the garage Echo in another Echo network: {r!r}")
    ha = house(); off = ECHOS[0]; off.online = False         # HA still lists the office, its link just dropped
    r, a = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through" and a.errors, f"the office's link down when called: {r!r} ({a.errors})")

    ha = house(); ECHOS[1].refuse = True                         # do not disturb, microphones off
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through" and ECHOS[0].state() == "idle", f"the kitchen refuses: {r!r}")
    ha = house(); ECHOS[1].refuse = "fast"                       # the same, Home Assistant never hears "calling"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through", f"the kitchen refuses before 'calling' is out: {r!r}")
    ha = house(); ECHOS[1].ask = True                            # Drop In answers: after the action button
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and ECHOS[0].state() == "calling" and ECHOS[1].state() == "ringing", f"the kitchen rings: {r!r}")

    # 6. the action under the second name only (model + MAC), under none: never "Done"
    ha = house(); off = ECHOS[0]
    ha.services["esphome.echo_dot_2_1cdf2e_drop_in"] = None; del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    ha.devices["dev_office"]["name"] = "Echo Dot 2"                  # name_make without a MAC: node "echo-dot-2"
    ha.services["esphome.echo_dot_2_drop_in"] = off.drop_in
    r, a = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and [c[0] for c in drop_actions(ha)] == ["esphome.echo_dot_2_drop_in"], f"office named 'Echo Dot 2': {r!r} {drop_actions(ha)}")
    ha = house(); off = ECHOS[0]
    del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    ha.devices["dev_office"]["name"] = "Büro"                       # a name that is not the node's
    ha.services["esphome.echo_dot_2_1cdf2e_drop_in"] = off.drop_in
    r, a = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche", f"action only under model + MAC: {r!r}")
    check([c[0] for c in drop_actions(ha)] == ["esphome.buero_drop_in", "esphome.echo_dot_2_1cdf2e_drop_in"] and
          any("service not found" in e for e in a.errors), f"  the first name not found in a run of its own, then the second: {drop_actions(ha)} {a.errors}")
    ha = house(); del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    r, a = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through", f"no action at all: {r!r} (errors {a.errors})")
    ha = house(); del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    r, a = ask(ha, "küche", "dev_office", this=False)
    check(r == "Done", f"no action, no 'this' (not as an automation): {r!r}, as Home Assistant would")
    ha = house(); r, a = ask(ha, "küche", "dev_office", this=False)
    check(r == "Calling Küche" and a.runs == 1, f"no 'this': the action called directly: {r!r}")

    # 7. what Home Assistant may hold differently: entity ids renamed, the sensor disabled or unavailable, other
    #    sensors of the Echo saying "idle" or "Connected", manufacturer "Amazon", a config entry titled "esphome"
    ha = house(); off = ECHOS[0]
    ha.entities["sensor.office_whatever"] = ha.entities.pop(off.sensor); ha.states["sensor.office_whatever"] = ha.states.pop(off.sensor)
    off.sensor = "sensor.office_whatever"
    ha.entity("sensor.office_bluetooth_speaker", "dev_office", "Connected", icon="mdi:speaker")
    ha.entity("sensor.office_status", "dev_office", "idle", icon="mdi:information")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and off.state() == "connected", f"office's sensor renamed, with 'idle' and 'Connected' sensors beside: {r!r}")
    ha = house(); off = ECHOS[0]
    ha.entities[off.sensor]["disabled"] = True; del ha.states[off.sensor]
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and drop_actions(ha), f"the office's Drop In sensor disabled: {r!r}")
    ha = house(); ha.devices["dev_office"]["manufacturer"] = "Amazon"; ha.devices["dev_kitchen"]["manufacturer"] = "Amazon"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche", f"manufacturer 'Amazon' (no project name): {r!r}")
    ha = house(); ha.devices["dev_living"]["manufacturer"] = "Someone"; ha.devices["dev_living"]["model"] = "Other"
    r, _ = ask(ha, "living room", "dev_office")
    check(r == "Calling Living Room", f"an Echo with neither maker nor model known, its Drop In sensor as hassmic's: {r!r}")
    ha = house(); ha.devices["dev_kitchen"]["manufacturer"] = "Someone"; ha.devices["dev_kitchen"]["model"] = "Other"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "There is no Echo in küche", f"  but not with its Drop In sensor renamed too: {r!r}")
    ha = house(); ha.entity("light.esphome_lamp", None, "on", platform="hue"); ha.entries["esphome"] = ["light.esphome_lamp"]
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche", f"a config entry titled 'esphome' (integration_entities' trap): {r!r}")
    ha = house(); ha.entities["assist_satellite.echo_2_000001_assist_satellite"]["area_id"] = "bedroom"
    r, _ = ask(ha, "bedroom", "dev_office")
    check(r == "Calling Bedroom", f"the living room Echo's satellite put in the bedroom: {r!r}")

    # 8. languages and replies
    ha = house(); r, _ = ask(ha, "küche", "dev_office", lang="de-CH")
    check(r == "Ich verbinde mit Küche", f"de-CH: {r!r}")
    ha = house(); r, _ = ask(ha, "salle", "dev_office", lang="fr")
    check(r == "Je ne trouve pas d'Echo pour salle", f"fr, nobody there: {r!r}")
    ha = house(); r, _ = ask(ha, "küche", "dev_office", lang="pt-BR")
    check(r == "Calling Küche", f"a language with no replies: {r!r}")
    two = {"de": ["Ich verbinde mit {room}", "Kein Echo für {room}"], "en": ["Calling {room}", "No Echo in {room}"]}
    ha = house(); r, _ = ask(ha, "küche", "dev_vpe", lang="de", inputs={"replies": two})
    check(r == "Only an Echo with Drop In on can drop in", f"replies set by hand with two entries, a later case: {r!r}")
    ha = house(); r, _ = ask(ha, "nirgends", "dev_office", lang="de", inputs={"replies": two})
    check(r == "Kein Echo für nirgends", f"  and their own second: {r!r}")
    ha = house(); r, _ = ask(ha, "küche", "dev_office", lang="de", inputs={"replies": {"de": "Verbinde {room}"}})
    check(r == "Verbinde Küche", f"a reply given as a string: {r!r}")
    ha = house(); r, _ = ask(ha, "", "dev_office", slots=False)
    check(r == "There is no Echo in", f"no room slot: {r!r}")

    # 9. Echos that say what came of a call ("Drop In result"): the reason, at once; a refusal is calling then idle
    def new_house(): ha = house(new=True); return ha, ECHOS
    ha, (off, kit, liv) = new_house(); r, a = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and off.state() == "connected" and ha.waited == 0 and not a.errors,
          f"new firmware, 'drop in küche': {r!r} (waits run out: {ha.waited} s)")
    check(ha.states["event.buero_ergebnis"]["attributes"]["event_type"] == "connected", "  its result found under an id renamed")
    for refuse, want in (("dnd", "Küche has do not disturb on"), ("muted", "The microphones are off in Küche"),
                         ("busy", "Küche is in a call"), ("off", "Drop In is off on Küche")):
        ha, (off, kit, liv) = new_house(); kit.refuse = refuse
        r, a = ask(ha, "küche", "dev_office")
        check(r == want and ha.waited == 0 and off.state() == "idle", f"  refused ({refuse}): {r!r} (waits run out: {ha.waited} s)")
    ha, (off, kit, liv) = new_house(); kit.ask = True
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and ha.waited == 0 and kit.state() == "ringing", f"  it rings there: {r!r}")
    ha, (off, kit, liv) = new_house(); kit.online = False                      # HA still has it, it is gone
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Küche did not answer", f"  no answer, HA still has it online: {r!r}")
    ha, (off, kit, liv) = new_house(); kit.online = False; ha.set_state(kit.sensor, "unavailable")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Küche is offline", f"  no answer, offline in HA: {r!r}")
    ha, (off, kit, liv) = new_house(); kit.network = "other"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Küche is not in this Echo's network" and ha.waited == 0 and "calling" not in [s for e, s in ha.pending],
          f"  in another Echo network: {r!r}")
    ha, (off, kit, liv) = new_house(); off.muted = True
    r, _ = ask(ha, "küche", "dev_office", lang="de")
    check(r == "Die Mikrofone dieses Echos sind aus", f"  this Echo muted (HA not told yet), German: {r!r}")
    ha, (off, kit, liv) = new_house(); off.busy = True
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "This Echo is in a call already", f"  this Echo in a call HA has not heard of: {r!r}")
    two = {"en": ["Calling {room}", "No Echo in {room}"]}
    ha, (off, kit, liv) = new_house(); kit.refuse = "dnd"
    r, _ = ask(ha, "küche", "dev_office", inputs={"replies": two})
    check(r == "Küche has do not disturb on", f"  replies set by hand before: the new ones in English: {r!r}")
    # old and new together: an old callee refuses with the same words; an old caller is waited for as before
    ha = house(); ECHOS[0] = off = Echo(ha, "dev_office", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:1C:DF:2E", "office", new=True)
    ECHOS[1].refuse = "dnd"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Küche has do not disturb on" and ha.waited == 0, f"  new caller, released callee refusing: {r!r}")
    ha, (off, kit, liv) = new_house(); ECHOS[0] = off = Echo(ha, "dev_office", "echo-dot-2", "Echo Dot 2", "AA:BB:CC:1C:DF:2E", "office")
    del ha.states["event.buero_ergebnis"]; del ha.entities["event.buero_ergebnis"]
    kit.refuse = "fast"
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through" and ha.waited == 4, f"  released caller, a refusal before 'calling': {r!r} after {ha.waited} s")
    # the action under the second name only: what the first costs
    ha, (off, kit, liv) = new_house()
    del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    ha.devices["dev_office"]["name"] = "Büro"
    ha.services["esphome.echo_dot_2_1cdf2e_drop_in"] = off.drop_in
    r, a = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche" and ha.waited == 2, f"  first action name not found: {r!r}, {ha.waited} s for it (released Echos: 4 s)")
    ha, (off, kit, liv) = new_house(); del ha.services["esphome.echo_dot_2_1cdf2e_drop_in"]
    r, a = ask(ha, "küche", "dev_office")
    check(r == "The call to Küche did not go through" and ha.waited == 6,
          f"  no action at all: {r!r} after {ha.waited} s (the last name waits long: a slow link is no failure)")
    ha, (off, kit, liv) = new_house(); ha.set_state("event.buero_ergebnis", "unavailable")
    r, _ = ask(ha, "küche", "dev_office")
    check(r == "Calling Küche", f"  its result entity unavailable at first: {r!r}")

    # 10. the model + MAC name rests on HA's model (project_name's second half) being the slug of the default name
    for f in sorted(glob.glob(os.path.join(ROOT, "devices", "*", "board.c"))):
        src = open(f).read()
        project = re.search(r'\.project\s*=\s*"([^"]*)"', src).group(1)
        name = re.search(r'\.default_name\s*=\s*"([^"]*)"', src).group(1)
        node = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")       # main.c node_of, ASCII
        check(project.split(".")[0] == "hassmic" and project.split(".")[1] == node,
              f"{os.path.relpath(f, ROOT)}: project {project} = hassmic.<slug of {name!r}>")
        check(node in open(BLUEPRINT).read(), f"  model {node} among the blueprint's known models")

    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
