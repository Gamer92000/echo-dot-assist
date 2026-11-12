"""hassmic cards: names shared by its parts."""
DOMAIN = "hassmic_cards"

CONF_TIMEOUT = "timeout"                    # s a voice request's card stays once the Echo is idle again
CONF_ROOM_ENTITIES = "room_entities"        # rows at most on a voice request's card
CONF_WEATHER_TOOLS = "weather_tools"        # LLM tools by these names answer about the weather (a regex)
CONF_LIBRARY = "library"                    # the dashboard (its URL) whose views are named cards ("": none)
CONF_VOICE = "voice_cards"                  # cards for voice requests at all (each Show also has its own switch)
CONF_ADVANCED = "advanced"                  # (the options form's collapsed section)
DEFAULTS = {CONF_TIMEOUT: 15, CONF_ROOM_ENTITIES: 8, CONF_WEATHER_TOOLS: "weather|forecast", CONF_LIBRARY: "echo-cards",
            CONF_VOICE: True}

# the Echo's events (hassmic: cards.c, proto_esphome.c), and ours
ECHO_ACTION = "esphome.hassmic_card_action"     # a button pressed: {card, action} (+ device_id, Home Assistant's)
ECHO_STATE = "esphome.hassmic_card_state"       # what the screen shows: {card, title, state: shown|gone}
ECHO_REFUSED = "esphome.hassmic_card_refused"   # a card the Echo did not take: {reason}
ACTION_EVENT = "hassmic_cards_action"           # a button pressed on a card of ours: {action, card, device_id}
REFUSED_EVENT = "hassmic_cards_refused"         # a card an Echo refused: {reason, device_id} (the logbook says it)

SIGNAL_DEVICE = "hassmic_cards_device_{}"       # dispatcher: an Echo's event (its device id): (kind, data)
SIGNAL_NEW_SHOW = "hassmic_cards_new_show"
