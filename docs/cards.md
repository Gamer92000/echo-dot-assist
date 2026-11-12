# Cards on the Echo Show's screen

Two layers:

1. **What people write**: Home Assistant's own dashboard cards, given to the `hassmic_cards` integration
   (`ha/custom_components/hassmic_cards`, blueprints in `ha/blueprints/automation/hassmic_cards`). Its action, entities,
   device triggers, events and notify entities are the interface automations use.
2. **The Echo's card**: the JSON `hassmic_cards` sends to an Echo's ESPHome action `show_card`, and the events the Echo
   fires back. The integration and the firmware are versioned together; the format is written down here so either side
   can be checked against it, not for automations to write by hand.

## 1. hassmic_cards

**Setting it up:** copy `ha/custom_components/hassmic_cards` into Home Assistant's `custom_components` folder, restart,
then Settings → Devices & services → Add integration → **Echo Show cards**. Its options (the same screen later):

| Option | Default | |
|---|---|---|
| Cards for voice requests | on | a card on the Show that was asked (each Show also has its own *Voice cards* switch) |
| Voice card timeout | 15 s | how long a voice request's card stays once the Echo is idle again |
| Rows on a voice card | 8 | what a request did, then the rest of its room, up to this many rows |
| Card library dashboard | `echo-cards` | the URL of the dashboard whose views are named cards (below) |
| LLM weather tools (*Advanced*) | `weather\|forecast` | a regular expression: an LLM tool whose name matches answers about the weather; checked when saved |

A `hassmic_cards:` block in `configuration.yaml` (the same keys) is imported once; a repair then asks to remove it.

**On each Echo Show** (a device of hassmic cards' own, named as the Echo, in its area):

- **Card button** (event entity, event type `pressed`, attributes `action`, `card`): a button pressed on one of its
  cards. In the automation editor: an *Entity* trigger on it, or the device trigger below.
- **Card on screen** (sensor): the id of the card it shows where an automation gave it one (else its title), `none`
  when it shows none; attributes `card`, `title`, `scope` (notice or request), `since`, and `refused` / `refused_at`:
  why and when it last refused a card (cleared when a card shows again). It hears what the Echo reports from when the
  integration is set up: a card shown before (during Home Assistant's start) shows as `none` until the next one.
- **Voice cards** (switch, configuration): cards for voice requests on this Show; off, automation cards still show.
- **Screen** (notify entity): `notify.send_message` puts a message card on it (title and words, 30 s).
- **Device trigger** "A button on a card pressed" (optionally only one button, by its action).
- All of them are unavailable while the Echo is (they follow its ESPHome entities).

And one notify entity for every Show, **All Echo Show screens**. *Download diagnostics* on the integration lists the
Shows it found, the cards out on them (without their picture tokens), the library's cards and the options.

A card an Echo refuses is in the logbook ("Echo Show 8 ... refused a card: ..."). The Echo reports what it shows and
refuses; a card it was sent that is gone (tapped away) takes no updates, which is no error.

**The card library:** create a dashboard (Settings → Dashboards, its URL `echo-cards` or the option's) and give it one
view per card: the view's URL is the card's name, its first card is the card, built and previewed with Home
Assistant's own card editor. `hassmic_cards.show` with `card: laundry` shows the view `laundry`'s card. Its strings may
be templates of the action's `variables` (`title: "{{ who }} is done"`); a markdown card's `content` is a template
anyway, rendered with them too, and follows the entities it names. A card changed in the library changes the cards
out on the Echos at their next update.

**Blueprints** (`ha/blueprints/automation/hassmic_cards`, copy into `blueprints/automation/`):
- *doorbell camera*: a doorbell (event entity, binary sensor or button) rings → the camera's live card on the Shows,
  optionally a chime and words on voice satellites;
- *appliance done*: a power sensor low for a while after it ran (above *Running above* just before; a restart or an
  unavailable sensor is no cycle) → a card with "Remind me" (the card snoozed: back after a while) and "Done",
  optionally words outside quiet hours (overnight or within a day); its words are inputs, for other languages;
- *answer a question with a card*: sentences spoken to an Echo Show → an answer and a card (a dashboard card or a
  library name) on the Echo that heard them.

### hassmic_cards.show

```yaml
action: hassmic_cards.show
target:                    # Echo devices, or areas, floors, labels (their Shows); none: every Echo Show. A target
                           # that names no Show is an error
  device_id: <echo device id>
data:
  card:                    # a dashboard card, as a dashboard takes it; or a library card's name (card: laundry)
    type: entities
    entities: [light.kitchen, {entity: switch.coffee, name: Coffee}]
  variables: {minutes: 52} # optional: for the card's templates
  id: kitchen              # optional: the same id again updates the card in place; clear takes it by id
  title: Kitchen           # optional: in place of the card's own
  subtitle: Turned on the light
  message: ...             # optional: text beside a picture card's picture
  timeout: 30              # s; 0: until tapped away or cleared
  scope: notice            # notice (an automation's) | request (a voice request's, see below)
  conversation_id: ...     # optional, request scope: only for this conversation
  actions:                 # buttons, as a phone notification's: up to 3
    - action: LAUNDRY_SNOOZE
      title: Remind me in 15 min
      perform_action: script.laundry_remind   # optional: run by Home Assistant when pressed
      data: {appliance: washer}
      keep: false          # false: the card goes (from every Echo it is on) after a press
      snooze: 15           # optional, minutes: the press takes the card off its Echos and brings it back after them
```

It answers (`response_variable`) with `id` (the card's: the one given, else one made up), `shown_on` (the Echos' nodes
it went to) and `failed` (those that refused it or could not be reached).

### hassmic_cards.show_message

A message card from plain fields, for the automation editor: `title`, `message`, `subtitle`, a `camera` (its picture,
moving while it shows unless `live: false`) or an `image` URL, `id`, `timeout`, and up to three buttons (`button_1`:
its label, `button_1_action`: the id a press fires `hassmic_cards_action` with, made from the card's id when empty,
`button_1_snooze`: minutes). The same target and answer as `show`. Its words are shown as written, never rendered as
a template.

Cards an Echo Show draws, and how:

| Dashboard card | On the Echo | Notes |
|---|---|---|
| `entities` | a row per entity (up to 8) | a control by domain (below); `type: simple-entity` rows only show; `focus: true` marks a row (extension) |
| `glance` | rows, none with a control | |
| `tile` | one row, marked | |
| `weather-forecast` | now, the days after | `forecast_type` daily or twice_daily (day and night periods merged into days); `show_forecast` |
| `markdown` | a message | `content` is a template, rendered once when shown and again at each update; plain text |
| `picture-entity` | a message with the picture | a camera's snapshot (`camera_view: live`: a new one every `refresh` s, default 2); another entity's `entity_picture` and state |
| `picture-glance` | a message with the picture | `camera_image` or `image`; `entities` as "Name: state" text |
| `picture` | a message with the picture | `image`: a URL, `/local/...` made Home Assistant's own |
| `todo-list` | a list of what is to do (up to 12) | |
| `calendar` | a list of events (up to 12) | `entities`; `days` (1) and `offset` (0: today, 1: tomorrow): extensions |

Controls on `entities` rows, by domain: a light's switch (and its brightness where it dims), the switch of switch,
input_boolean, fan, automation, humidifier, siren; a cover's open/stop/close; a thermostat's setpoint down/up (its
`target_temp_step`); a TV's or receiver's power, another player's play/pause; Run for scene, script, button. Never a
lock's or an alarm panel's (the Echo refuses them too), and a garage, gate or door cover's only with
`secure_controls: true` on the card. An entity that is unavailable has none.

Every word is Home Assistant's: its translations of states ("On", "Open", "Heat"), numbers at the entity's display
precision with their unit, and language-neutral forms where Home Assistant has no words ("20.5° → 21°", "↑23° ↓12°").
Weekdays on a forecast and a calendar's day come from a table of 13 languages (Home Assistant's language; English
otherwise), times are 12- or 24-hour by Home Assistant's country (Home Assistant has no date formatting of its own on
the server).

**Live:** while a card may show (its timeout and a margin; 12 h for one until tapped away, from when the Echo last
said it shows it), a change of one of its entities sends it again (at most once a second). After those 12 h a card until
tapped away with buttons keeps only its buttons (for 30 days): "Remind me" the next morning still works, but it is no
longer updated and its camera picture has ended and the Echo replaces it in place. A tap on a control shows its result at once; the update
says how it went. Cards are kept across a Home Assistant restart: their buttons and pictures go on working, and their
entities are followed again once Home Assistant runs.

**Scope:** a notice shows from when it comes. Up to 4 are kept: the newest shows, and one under it comes back when the
newer goes (the doorbell over the laundry's card). A voice request hides them while it runs and while the request's own
card shows; then they come back. A notice that comes after the request, while its card still shows, shows over it at
once (the request's card back under it). A new notice wakes the screen; a ringing timer comes before any card (with
"Timers on screen" off, the face's ring and Stop show, the card back after). A request's card (`scope: request`) shows only on an Echo
whose request is running or just ended (10 s; the end of an announcement is no request), only for that conversation
when one is named, and stays the timeout after the Echo is idle again; the next request takes it away. A sentence-triggered automation shows its answer's card like this:

```yaml
- action: hassmic_cards.show
  target: {device_id: "{{ trigger.device_id }}"}
  data:
    scope: request
    card: {type: entities, entities: [input_boolean.coffee_maker]}
```

### Buttons: hassmic_cards_action

A press fires `hassmic_cards_action` with `action` (the button's), `card` (the card's id) and `device_id` (the Echo's),
the way `mobile_app_notification_action` does for a phone; an automation takes it from there. A button with
`perform_action` also runs that (its `data` as rendered when the card was shown; a failure is logged, the card still
goes). Only a press reported for an Echo Show counts, and for a card the integration knows only for an Echo it was sent
to (any ESPHome device can fire `esphome.*` events; Home Assistant lets a device's own event data name a `device_id`, so
this keeps other devices' presses out by mistake, not an adopted device that means to). A button without `keep` takes
its card off every Echo it was sent to, so a card on two Shows is answered once (a press on the other one in the
seconds before its card goes is ignored). A button
with `snooze` takes it off and sends it again after that many minutes (kept across a restart); its press fires the
event and runs its `perform_action` like any other (an automation that takes every button can tell it by its
`action`). Card and action ids are 63 bytes at most (the Echo keeps no more).

### hassmic_cards.clear, notify entities

- `hassmic_cards.clear` (`id`; `target` optional): that card wherever it is (or on the target's Shows); no id: whatever
  shows on the target's Shows.
- `notify.send_message` on a Show's *Screen* entity or *All Echo Show screens* (`title`, `message`): a message card
  for 30 s from anything that notifies. Its message is shown as sent, never rendered as a template (the voice path's
  answers neither). Pictures and buttons: `show_message`.

### Voice requests

The integration reads each conversation's chat log (`conversation.chat_log.async_subscribe_chat_logs`, a Home Assistant
internal it checks at start; a repair issue says when it is missing). When a turn's answer comes (the assistant's
content without tool calls), the card goes to the Echo whose assist satellite holds the conversation id, as a request's
card: what was done (those entities marked, the rest of the room after them), else the weather, else what was asked
about (`HassGetState`, the LLM's `GetLiveContext` by name), else, if something failed, the answer itself. A new
request in the same conversation before the answer drops the turn. The conversation id lasts a few minutes across
requests, so it narrows a late card down rather than identifying its request. Cards for voice requests are off where
the option or the Show's *Voice cards* switch says so.

**Pictures:** the Echo has no Home Assistant login, so a camera's snapshot is served at
`/api/hassmic_cards/camera/<token>`, a random token per card that lasts as long as the card may show (one snapshot a
second at most per camera, shared by the Shows asking). hassmic leaves the token out of its logs, as it leaves out
every URL's query (Home Assistant's signed URLs and entity pictures carry their tokens there).

**Tests:** `tests/ha_real_cards.py` runs all of this against a real Home Assistant and a PC Echo Show (CI's
home-assistant job).

## 2. The Echo's card (version 2)

ESPHome actions of an Echo Show: `show_card` (`card`: the JSON below, at most 8 KB), `clear_card` (`id`; empty: the card
that shows). Events (all need no permission; Home Assistant adds `device_id`): `esphome.hassmic_card_state` (`card`,
`title`, `state` shown|gone: on every change of what the screen shows, and again when Home Assistant subscribes),
`esphome.hassmic_card_refused` (`reason`), and the button's `esphome.hassmic_card_action` (`card`, `action`). A
control's tap runs a Home Assistant action, which needs "Allow the device to perform Home Assistant actions".

```json
{"v": 2, "id": "laundry", "update": false,
 "type": "entities" | "weather" | "message" | "list",
 "title": "...", "subtitle": "...",
 "scope": "notice" | "request", "conversation_id": "...", "timeout": 30,
 "entities": [{"entity_id": "light.kitchen", "name": "Light", "value": "On · 80 %",
               "control": "read" | "toggle" | "dim" | "cover" | "climate" | "play" | "run",
               "on": true, "level": 80, "setpoint": 21.5, "step": 0.5, "focus": true}],
 "weather": {"condition": "sunny", "temperature": "21°", "detail": "↑23° ↓12°",
             "forecast": [{"label": "Fri", "condition": "rainy", "temps": "18° / 11°", "precip": "60 %"}]},
 "message": "...", "image": "http://...", "image_refresh": 2,
 "items": [{"text": "Dentist", "detail": "3:00 PM · Family"} | "Milk"],
 "buttons": [{"label": "Remind me", "id": "SNOOZE", "keep": false}]}
```

- `v` must be 2; another is refused (the Echo's log says so). A `type` the Echo does not know shows as a message (its
  title and message) when it has one, else is refused.
- `update`: only replaces the card with that id while it shows (its picture and time kept); refused when it is gone.
- `control`: read-only unless given, and only where it fits the entity's domain; a lock's and an alarm panel's never.
  `toggle` (light, switch, input_boolean, fan, automation, humidifier, siren, media_player only) runs `<domain>.toggle`, `dim` also `light.turn_on brightness_pct`, `cover` `cover.open/stop/close_cover`,
  `climate` `climate.set_temperature setpoint ± step`, `play` `media_player.media_play_pause`, `run`
  `scene|script.turn_on` / `button.press`. `on` and `level` are what the control shows; `value` is the row's words.
- Limits: 8 rows, 5 days, 12 items, 3 buttons; strings are cut at character boundaries to the Echo's buffers (a name
  63 bytes, a row's words 63, a title 127, a message 599, an item 95 and its detail 63).
- `timeout`: s; notices from when they came (default 30), requests from idle (default 15); 0 until tapped away or
  cleared; a negative one is the default. The screen stays on while a card shows, 10 minutes at most (the card stays
  until gone).
- `image`: http or https only, fetched by the Echo when it shows (`image_refresh`: s between fetches, 0 once; refreshes
  stop after 3 failed fetches in a row, and for a card until tapped away when it stops holding the screen on).
- The Echo takes cards from any client of its ESPHome API that it would take any other action from: over the encrypted
  link once Home Assistant has set its key (at adoption), from any client before that.
