/* BlueZ pairing agent for the speaker.  The controller is discoverable only
 * during the pairing window; already paired phones may reconnect afterwards. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
extern DBusConnection *dbus_bus_get_private(int, void *);
extern int dbus_bus_request_name(DBusConnection *, const char *, unsigned, void *);
extern void dbus_connection_set_exit_on_disconnect(DBusConnection *, int);
extern int dbus_connection_read_write(DBusConnection *, int);
extern DBusMessage *dbus_connection_pop_message(DBusConnection *);
extern DBusMessage *dbus_connection_send_with_reply_and_block(DBusConnection *, DBusMessage *, int, void *);
extern int dbus_connection_send(DBusConnection *, DBusMessage *, unsigned *);
extern void dbus_connection_flush(DBusConnection *);
extern DBusMessage *dbus_message_new_method_call(const char *, const char *, const char *, const char *);
extern DBusMessage *dbus_message_new_method_return(DBusMessage *);
extern DBusMessage *dbus_message_new_error(DBusMessage *, const char *, const char *);
extern int dbus_message_append_args(DBusMessage *, int, ...);
extern const char *dbus_message_get_member(DBusMessage *);
extern int dbus_message_is_method_call(DBusMessage *, const char *, const char *);
extern void dbus_message_unref(DBusMessage *);

static volatile sig_atomic_t stop;
static void stop_handler(int sig) { (void)sig; stop = 1; }
static int manager_call(DBusConnection *c, const char *method, int capability)
{
    const char *path = "/org/l09g/agent", *cap = "NoInputNoOutput";
    DBusMessage *m = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", method);
    if (!m) return -1;
    int ok = capability ? dbus_message_append_args(m, 'o', &path, 's', &cap, 0)
                        : dbus_message_append_args(m, 'o', &path, 0);
    DBusMessage *reply = ok ? dbus_connection_send_with_reply_and_block(c, m, 5000, NULL) : NULL;
    dbus_message_unref(m);
    if (!reply) return -1;
    dbus_message_unref(reply);
    return 0;
}
int main(void)
{
    DBusConnection *c = dbus_bus_get_private(1, NULL); /* DBUS_BUS_SYSTEM */
    if (!c) { fprintf(stderr, "bt-agent: cannot connect to system bus\n"); return 1; }
    dbus_connection_set_exit_on_disconnect(c, 0);
    if (manager_call(c, "RegisterAgent", 1) || manager_call(c, "RequestDefaultAgent", 0)) {
        fprintf(stderr, "bt-agent: cannot register with BlueZ\n"); return 1;
    }
    signal(SIGTERM, stop_handler); signal(SIGINT, stop_handler);
    fprintf(stderr, "bt-agent: registered\n");
    while (!stop && dbus_connection_read_write(c, 1000)) {
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(c))) {
            const char *member = dbus_message_get_member(m);
            if (member && dbus_message_is_method_call(m, "org.bluez.Agent1", member)) {
                int accept = !strcmp(member, "RequestAuthorization") || !strcmp(member, "AuthorizeService") ||
                             !strcmp(member, "RequestConfirmation") || !strcmp(member, "Cancel") || !strcmp(member, "Release");
                DBusMessage *reply = accept ? dbus_message_new_method_return(m) :
                    dbus_message_new_error(m, "org.bluez.Error.Rejected", "Unsupported pairing method");
                if (reply) { dbus_connection_send(c, reply, NULL); dbus_connection_flush(c); dbus_message_unref(reply); }
                fprintf(stderr, "bt-agent: %s %s\n", member, accept ? "accepted" : "rejected");
                if (!strcmp(member, "Release")) stop = 1;
            }
            dbus_message_unref(m);
        }
    }
    return 0;
}
