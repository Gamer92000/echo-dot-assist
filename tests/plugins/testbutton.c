/* A stand-in for hassmic's own tests (tests/fake_plugins_button.py): what `button` and rows of its own give a plugin, used as an
 * automation's plugin would, against the SDK's client.  `button`: every BUTTON goes into its log ("plugin testbutton:
 * button <key> <how>", the protocol's numbers: 1 the action button, 2 volume up, 3 volume down; 1 pressed, 2 held), for
 * the test to read whether a press reached it.  Rows of its own (`ha.entities` marked "page"): how many presses it heard
 * ("presses"), the last one in words ("last"), whether the action button was held last ("held"); and one entity Home
 * Assistant gets that is no row ("quiet"), set once.  It sends nothing else and asks for nothing. */
#include "hassmic_plugin.h"
#include <stdio.h>

static int presses;

static void show(const char *last, int held)
{
    char n[16]; snprintf(n, sizeof n, "%d", presses);
    hm_entity_state("presses", n); hm_entity_state("last", last); hm_entity_state("held", held ? "on" : "off");
}

/* (states only once it runs: hassmic takes none before READY, which the client sends after the ready callback) */
static void on_tick(void *ctx) { static int once; (void)ctx; if (!once++) { show("none yet", 0); hm_entity_state("quiet", "7.5"); } }

static void on_button(void *ctx, int key, int how)
{
    static const char *const keys[] = { "?", "action", "volume up", "volume down" };
    char last[64]; (void)ctx;
    hm_log("button %d %d", key, how);
    presses++;
    snprintf(last, sizeof last, "%s %s", keys[key >= 1 && key <= 3 ? key : 0], how == HM_BUTTON_HOLD ? "held" : "pressed");
    show(last, key == HM_BUTTON_ACTION && how == HM_BUTTON_HOLD);
}

int main(void)
{
    static const struct hm_callbacks cb = { .tick = on_tick, .button = on_button };
    return hm_run(&cb, NULL) ? 1 : 0;
}
