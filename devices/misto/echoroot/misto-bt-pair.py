#!/usr/bin/python
"""BlueZ 4 pairing window, called through hassmic's bounded helper."""
import sys
import dbus

try:
    bus = dbus.SystemBus()
    manager = dbus.Interface(bus.get_object('org.bluez', '/'), 'org.bluez.Manager')
    adapter = dbus.Interface(bus.get_object('org.bluez', manager.DefaultAdapter()), 'org.bluez.Adapter')
    if len(sys.argv) == 5 and sys.argv[1] == '-i':
        on = sys.argv[4] == '1'
        if on:
            # A switch must not report off while Pairable remains enabled after
            # BlueZ's discoverability timer expires. It stays open until closed.
            adapter.SetProperty('DiscoverableTimeout', dbus.UInt32(0, variant_level=1), timeout=1)
            adapter.SetProperty('PairableTimeout', dbus.UInt32(0, variant_level=1), timeout=1)
        # Closing inquiry first avoids advertising a window that is already shut.
        changes = [('Pairable', True), ('Discoverable', True)] if on else [('Discoverable', False), ('Pairable', False)]
        try:
            for prop, val in changes:
                adapter.SetProperty(prop, dbus.Boolean(val, variant_level=1), timeout=1)
        except Exception:
            if on:
                for prop in ('Discoverable', 'Pairable'):
                    try:
                        adapter.SetProperty(prop, dbus.Boolean(False, variant_level=1), timeout=1)
                    except Exception:
                        pass
            raise
    props = adapter.GetProperties(timeout=1)
    print(int(bool(props['Pairable']) and bool(props['Discoverable'])))
except Exception as e:
    sys.stderr.write('Bluetooth adapter error: %s\n' % e)
    sys.exit(1)
