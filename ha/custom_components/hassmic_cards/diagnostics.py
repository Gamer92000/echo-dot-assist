"""Diagnostics of hassmic cards (Settings → Devices & services → Echo Show cards → Download diagnostics): the Echo Shows
it found, the cards out on them (tokens left out), the last thing each Show reported, the card library, and whether the
Home Assistant internals voice cards rely on are there."""
from __future__ import annotations

import time
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant


async def async_get_config_entry_diagnostics(hass: HomeAssistant, entry: ConfigEntry) -> dict[str, Any]:
    cards = entry.runtime_data
    now = time.time()
    library: Any
    try:
        from homeassistant.components.lovelace.const import LOVELACE_DATA
        dash = hass.data[LOVELACE_DATA].dashboards.get(cards.render.library)
        conf = await dash.async_load(False) if dash else None
        library = {"dashboard": cards.render.library, "found": dash is not None,
                   "cards": [v.get("path") or v.get("title") for v in (conf or {}).get("views") or []]}
    except Exception as err:                                    # noqa: BLE001
        library = {"dashboard": cards.render.library, "error": str(err)}
    return {
        "options": dict(entry.options),
        "voice_cards": {"internals_present": cards.voice_ok, "on": cards.voice, "off_on": sorted(cards.voice_off)},
        "shows": [{"node": n, "device_id": d, "show_card": hass.services.has_service("esphome", f"{n}_show_card"),
                   "reported": cards.state.get(d)} for n, d in cards.shows()],
        "live_cards": [{"id": c["id"], "named": c.get("named"), "echos": c["echos"], "type": c["cfg"].get("type"),
                        "name": c["cfg"].get("name"), "scope": c["opts"].get("scope", "notice"), "timeout": c["opts"].get("timeout"),
                        "expires_in_s": round(c["expires"] - now), "snoozed": bool(c.get("snoozed_until")),
                        "buttons": [a.get("action") for a in c["opts"].get("actions") or []], "pictures": len(c.get("pictures") or [])}
                       for c in cards.live.cards.values()],
        "library": library,
    }
