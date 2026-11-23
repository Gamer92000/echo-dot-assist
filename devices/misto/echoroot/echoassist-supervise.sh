#!/bin/sh
BASE=${ECHOASSIST_HOME:-/home/root/echo1-assist}
RUN=${ECHOASSIST_RUN:-/var/run/echoassist}
LOG=${ECHOASSIST_LOG:-/tmp/echoassist.log}
child=
cleaned=0
cleanup() {
    [ "$cleaned" = 0 ] || return
    cleaned=1
    if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
        kill -TERM "$child" 2>/dev/null
        n=0
        while kill -0 "$child" 2>/dev/null && [ "$n" -lt 10 ]; do sleep 1; n=$((n+1)); done
        kill -KILL "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    rm -f "$RUN/child" "$RUN/pid"
    rmdir "$RUN" 2>/dev/null || :
}
trap '' HUP
trap 'exit 0' TERM INT
trap cleanup EXIT
printf '%s\n' "$$" > "$RUN/pid"
# A log in volatile /tmp; retain at most ~256 KiB plus a temporary tail.
rotate() {
    [ -f "$LOG" ] || return
    nbytes=$(wc -c < "$LOG")
    if [ "$nbytes" -gt 262144 ]; then
        tail -c 131072 "$LOG" > "$LOG.trim" && cat "$LOG.trim" > "$LOG"
        rm -f "$LOG.trim"
    fi
}
while :; do
    if [ "${ECHOASSIST_SKIP_READY:-0}" != 1 ]; then
        while [ ! -e /dev/shm/BMicsOut.shm ] || [ ! -S /var/run/pulse/native ]; do sleep 2; done
    fi
    rotate
    printf '\n[service] starting hassmic\n' >> "$LOG"
    /bin/sh "$BASE/run-local.sh" >> "$LOG" 2>&1 &
    child=$!
    printf '%s\n' "$child" > "$RUN/child"
    while kill -0 "$child" 2>/dev/null; do
        if [ -r "$BASE/misto-wifi-scan.py" ]; then
            HASSMIC_STATE="$BASE/state-local" HASSMIC_WIFI="$BASE/wifi-local" /usr/bin/python "$BASE/misto-wifi-scan.py" >> "$LOG" 2>&1
        fi
        sleep "${ECHOASSIST_POLL:-5}"
        rotate
    done
    wait "$child"; rc=$?
    child=
    rm -f "$RUN/child"
    printf '[service] hassmic exited (%s); retry in 3 seconds\n' "$rc" >> "$LOG"
    sleep "${ECHOASSIST_RETRY:-3}"
done
