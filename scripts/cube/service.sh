#!/system/bin/sh
# Cube Assist only. No flashing, system remounts, network changes or stock-service kills.
ROOT=${CUBE_ASSIST_HOME:-/data/local/cube-assist}
case "$ROOT" in /data/local/cube-assist|/data/local/tmp/cube-assist-trial) ;; *) echo 'Unexpected install directory' >&2; exit 1 ;; esac
export CUBE_ASSIST_HOME="$ROOT" HASSMIC_STATE="$ROOT/state" HASSMIC_MODELS="$ROOT/models" HASSMIC_LOG="$ROOT/boot.log"
export CUBE_AUDIO_SOCKET=cube-assist-audio
# Frequent status heartbeats belong on Android tmpfs, not persistent flash.
export CUBE_ROUTING_STATUS=/dev/cube-assist-routing-status
export HASSMIC_MDNS_FILE="$ROOT/services.xml"
export CLASSPATH="/system/framework/aspclient.jar:$ROOT/current/classes.dex"
kill_owned() {
    file=$1 expected=$2
    [ -f "$ROOT/$file" ] || return
    pid=$(cat "$ROOT/$file")
    case "$pid" in ''|*[!0-9]*) return ;; esac
    if [ -r "/proc/$pid/cmdline" ] && grep -aq "$expected" "/proc/$pid/cmdline"; then kill "$pid" 2>/dev/null; fi
}
cleanup() {
    kill_owned native.pid "$ROOT/current/hassmic"
    kill_owned audio.pid CubeAudio
    sleep 1
    timeout 12 app_process / CubeAudio reset >> "$ROOT/guard.log" 2>&1
    # Settings restoration remains independent of the Java runtime.
    original=$(cat "$ROOT/original-wake-setting" 2>/dev/null)
    case "$original" in 0|1) settings put secure alexa_wake_word_disabled "$original" ;; null) settings delete secure alexa_wake_word_disabled ;; esac
}
case "${1:-status}" in
start)
    [ -f "$ROOT/current/hassmic" ] && [ -f "$ROOT/current/classes.dex" ] || exit 1
    if ! mkdir "$ROOT/run.lock" 2>/dev/null; then
        pid=$(cat "$ROOT/supervisor.pid" 2>/dev/null)
        case "$pid" in ''|*[!0-9]*) pid=0 ;; esac
        if [ "$pid" -gt 0 ] && [ -r "/proc/$pid/cmdline" ] && grep -aq "$ROOT/service.sh" "/proc/$pid/cmdline"; then echo 'Already running';exit 0;fi
        rmdir "$ROOT/run.lock" 2>/dev/null && mkdir "$ROOT/run.lock" || exit 1
    fi
    rm -f "$ROOT/stop"
    nohup sh "$ROOT/service.sh" supervise "${2:-0}" > "$ROOT/supervisor.log" 2>&1 < /dev/null &
    echo "$!" > "$ROOT/supervisor.pid"
    echo 'Cube Assist supervisor started'
    ;;
supervise)
    lease=${2:-0};case "$lease" in ''|*[!0-9]*) exit 1 ;; esac
    start=$(cut -d. -f1 /proc/uptime)
    trap 'cleanup; rmdir "$ROOT/run.lock" 2>/dev/null' EXIT
    trap 'touch "$ROOT/stop"; exit 0' TERM INT
    tries=0
    while [ ! -f "$ROOT/stop" ]; do
        cleanup
        rm -f "$ROOT/state/restart" "$ROOT/state/reboot"
        if [ -f "$ROOT/state/reset" ]; then
            stamp=$(cut -d. -f1 /proc/uptime)
            mv "$ROOT/state" "$ROOT/state-before-reset-$stamp" || exit 1
            mkdir -m 700 "$ROOT/state" || exit 1
        fi
        protocol=$(cat "$ROOT/protocol" 2>/dev/null);case "$protocol" in esphome|wyoming) ;; *) protocol=esphome ;; esac
        port=26053;[ "$protocol" = wyoming ] && port=10700
        [ ! -f "$ROOT/boot.log" ] || mv "$ROOT/boot.log" "$ROOT/boot.previous.log"
        "$ROOT/current/hassmic" -P "$protocol" -p "$port" -z 28928 -S > "$ROOT/services.xml.new" && mv "$ROOT/services.xml.new" "$ROOT/services.xml"
        [ ! -f "$ROOT/audio.log" ] || mv "$ROOT/audio.log" "$ROOT/audio.previous.log"
        app_process / CubeAudio > "$ROOT/audio.log" 2>&1 &
        audio_pid=$!;echo "$audio_pid" > "$ROOT/audio.pid"
        ready=0
        for n in 1 2 3 4 5 6 7 8 9 10; do
            if grep -q 'ready local socket=' "$ROOT/audio.log"; then ready=1;break;fi
            kill -0 "$audio_pid" 2>/dev/null || break
            sleep 1
        done
        [ "$ready" = 1 ] || { echo 'Audio helper failed';exit 1; }
        [ ! -f "$ROOT/boot.log" ] || mv "$ROOT/boot.log" "$ROOT/boot.previous.log"
        "$ROOT/current/hassmic" -P "$protocol" -p "$port" -z 28928 -a 28930 -i 28932 -W 28931 -o 0 -L -B -V > "$ROOT/boot.log" 2>&1 &
        native_pid=$!;echo "$native_pid" > "$ROOT/native.pid"
        while kill -0 "$native_pid" 2>/dev/null && kill -0 "$audio_pid" 2>/dev/null; do
            [ -f "$ROOT/stop" ] && exit 0
            now=$(cut -d. -f1 /proc/uptime)
            if [ "$lease" -gt 0 ] && [ "$((now-start))" -ge "$lease" ]; then echo 'Trial lease expired';exit 0;fi
            if [ -f "$ROOT/state/restart" ] || [ -f "$ROOT/state/reboot" ] || [ -f "$ROOT/state/reset" ]; then break;fi
            sleep 2
        done
        tries=$((tries+1));[ "$tries" -lt 5 ] || { echo 'Repeated failures; stopped with Alexa restored';exit 1; }
        sleep 2
    done
    ;;
stop)
    touch "$ROOT/stop"
    cleanup
    for n in 1 2 3 4 5 6 7 8 9 10;do [ ! -d "$ROOT/run.lock" ] && break;sleep 1;done
    if [ -d "$ROOT/run.lock" ];then
        kill_owned supervisor.pid "$ROOT/service.sh"
        sleep 2
        rmdir "$ROOT/run.lock" 2>/dev/null
    fi
    echo 'Cube Assist stopped; original Alexa wake setting restored'
    ;;
status)
    [ -d "$ROOT/run.lock" ] && echo 'Supervisor lock present' || echo 'Stopped'
    echo "Alexa wake disabled: $(settings get secure alexa_wake_word_disabled)"
    tail -10 "$ROOT/boot.log" 2>/dev/null
    ;;
*) echo 'Usage: service.sh start [trial_seconds] | stop | status' >&2;exit 2 ;;
esac
