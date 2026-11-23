#!/bin/sh
# Run EchoRoot's existing DHCP hook and signal a completed lease to the worker.
/usr/share/udhcpc/default.script "$1" || exit $?
case "$1" in
  bound|renew) [ -n "$MISTO_DHCP_MARKER" ] && [ -n "$ip" ] && printf '%s\n' "$ip" > "$MISTO_DHCP_MARKER" ;;
  deconfig|nak|leasefail) [ -n "$MISTO_DHCP_MARKER" ] && rm -f "$MISTO_DHCP_MARKER" ;;
esac
exit 0
