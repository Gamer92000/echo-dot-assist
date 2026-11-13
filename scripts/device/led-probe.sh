#!/system/bin/sh
# The light ring's driver measured from the Echo itself, for a live ring (feature/led-live): what ledcontroller writes
# into /sys/bus/i2c/devices/<bus>-<addr>/frame, how often, and whether a frame written by someone else stays.
# Run as root through scripts/led-probe.sh on the PC (pushed to /data/local/tmp, output kept in device-logs/).
#   led-probe.sh read    only reads: the driver's files, their modes and labels, ledcontroller, the mute PWM,
#                        the frame sampled for 10 s on an idle Echo.  Changes nothing.
#   led-probe.sh stock   only stock's own ways, as hassmic uses them every day: a few stock patterns played with
#                        ledctrl -s/-u and the frame sampled meanwhile; brightness 100/50/25/10/1 with ledctrl -a off -b
#                        and led_current read at each.  Afterwards the patterns unset and the brightness as before
#                        (hassmic's setting, else what the Echo showed).
#   led-probe.sh write   writes the frame file, with stock's own values only: what ledcontroller itself writes for
#                        solid_blue at brightness 100 (0000FD a LED; the driver scales it by the unit's calibration and
#                        reads back 0000A9, so only that measured case is taken), and the dark frame.  First that while
#                        stock shows it (no change to see); only if it reads back as stock's own:
#                        that frame on an idle ring (does it stay?), over a running pattern (is it overwritten?), and
#                        200 writes of the same frame in a row (how fast, how much CPU).  At the end, and on Ctrl-C:
#                        the dark frame, then a stock pattern set and unset so ledcontroller writes the frame itself.
#                        Nothing but the frame file is written; led_current, boot_animation and the mute PWM are only
#                        read.  sysfs is not kept across a reboot: a reboot puts back anything this could have left.
# The Echo must be idle (no timer, no reply, not muted, no light from Home Assistant on the ring) for stock and write.
# Run on a Dot 3 (donut) 2026-11-12: PLAN.md "Light ring live" has what it found.
B=/data/local/tmp/led-probe; mkdir -p $B
LR=/system/etc/led-resources
STAGE=${1:-read}

sec() { echo; echo "######## $*"; }
die() { echo "led-probe: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "not root"

# --- the driver's folder: the I2C device with a frame file (0-003f on the Dots measured so far)
F=; for f in /sys/bus/i2c/devices/*/frame; do [ -e "$f" ] && { F=$f; break; }; done
[ -n "$F" ] || die "no LED driver with a frame file under /sys/bus/i2c/devices (a screen model?)"
D=${F%/frame}

# --- tools: toybox's od, else xxd, else hexdump; fractional sleep
if od -A n -t x1 -v /proc/self/stat > /dev/null 2>&1; then hexof() { od -A n -t x1 -v "$1" | tr -d ' \n'; }
elif xxd -p /proc/self/stat > /dev/null 2>&1; then hexof() { xxd -p "$1" | tr -d '\n'; }
else hexof() { hexdump -v -e '1/1 "%02x"' "$1"; }; fi
sleep 0.05 2>/dev/null || die "this sleep takes no fractions"
now() { read -r u x < /proc/uptime; u=${u%.*}${u#*.}; NOW=$u; }          # centiseconds since boot
lc() { LC=$(pidof ledcontroller); LC=${LC%% *}; }
lc_cpu() { read -r l < /proc/$LC/stat; set -- $l; CPU=$(( ${14} + ${15} )); }      # ticks (1/100 s), user + system
lc_io() { SYSCW=0 WCHAR=0; while read -r k v; do case $k in syscw:) SYSCW=$v;; wchar:) WCHAR=$v;; esac; done < /proc/$LC/io; }
sys_cpu() { read -r c a b cc d e f g h x < /proc/stat; BUSY=$((a + b + cc + e + f + g + h)); TOT=$((BUSY + d)); }

# sample SECS LABEL: the frame read as fast as the shell can, each change printed with its time; ledcontroller's CPU and
# writes over the same time
sample() {
    lc; lc_cpu; c0=$CPU; lc_io; w0=$SYSCW; b0=$WCHAR
    now; t0=$NOW; end=$((t0 + $1 * 100)); n=0 ch=0 last=
    while now; [ $NOW -lt $end ]; do
        v=$(hexof $F); n=$((n + 1))
        [ "$v" = "$last" ] && continue
        ch=$((ch + 1)); last=$v
        [ $ch -le 40 ] && echo "  +$(( (NOW - t0) * 10 )) ms  $v"
    done
    lc_cpu; lc_io
    echo "  $2: $n reads in $1 s ($(( n / $1 ))/s), $ch distinct; ledcontroller $(( (CPU - c0) * 10 )) ms CPU," \
         "$((SYSCW - w0)) write calls, $((WCHAR - b0)) bytes written"
    LAST=$last
}

pats_active() { ledctrl -g 2>&1 | tr '\n' ' '; }
# lit FILE: any LED on?  The format is not known yet: text (hex or decimal numbers, "0x" prefixes) or bytes.  When in
# doubt it says lit, so stock and write stop rather than start on a ring that shows something
lit() {
    h=$(hexof $1)
    case $h in ''|*[!0-9a-f]*) return 0;; esac
    if [ "$(tr -d '\t\n\r -~' < $1 | wc -c)" -eq 0 ]; then         # printable: text
        t=$(cat $1 | sed 's/0[xX]//g' | tr -cd '0-9a-fA-F')
        case $t in *[1-9a-fA-F]*) return 0;; esac; return 1
    fi
    case $h in *[1-9a-f]*) return 0;; esac; return 1
}
idle_ok() {     # dark ring, nothing of stock's or ours showing, not muted
    lit $F && die "the ring seems lit ($(hexof $F)): wait until the Echo is idle, switch Home Assistant's light off (or the frame's format fools this check: send me the read stage's output)"
    [ "$(cat $PRIV 2>/dev/null)" = 1 ] && die "muted ($PRIV is 1): unmute first"
}
PRIV=/sys/devices/platform/gpio-privacy/state                                 # donut (board.c privacy_state)
[ -e $PRIV ] || PRIV=/sys/devices/soc/10010000.keypad/amz_privacy/state       # biscuit, radar

# a stock pattern that stays lit until unset, and one that moves
pick() { for p in "$@"; do [ -f $LR/$p.animation ] && { echo $p; return; }; done; }
SOLID=$(pick solid_blue solid_orange solid_red solid_green volume_step-30)
MOVING=$(pick active-thinking ready-timer)
SET=
unset_all() { for p in $SET; do ledctrl -u $p > /dev/null 2>&1; done; SET=; }
play() { ledctrl -s $1 > /dev/null 2>&1; SET="$SET $1"; }

# brightness back as hassmic keeps it (state/config), else as it was
CONF=/data/local/hassmic/state/config
saved_level=$(getprop persist.ledbrightness.bootup)
bright_restore() {
    a=$(sed -n 's/^led_auto_brightness=//p' $CONF 2>/dev/null); l=$(sed -n 's/^led_brightness=//p' $CONF 2>/dev/null)
    case $a in 0|off) a=0;; esac                                              # state/config says on/off
    if [ "$a" = 0 ]; then ledctrl -a off -b ${l:-$saved_level} > /dev/null 2>&1; echo "brightness: fixed at ${l:-$saved_level} again"
    else ledctrl -a on > /dev/null 2>&1; echo "brightness: auto again (hassmic's setting${a:+ $a})"; fi
}

echo "led-probe $STAGE: $(getprop ro.product.device) $(getprop ro.build.display.id), driver $D"

case $STAGE in
read)
    sec "driver files: modes, owners, SELinux labels"
    ls -laZ $D/ 2>&1
    ls -laZ $F $D/led_current $D/boot_animation 2>&1
    echo "name: $(cat $D/name 2>&1)"; echo "driver: $(readlink $D/driver 2>&1)"
    echo "compatible: $(cat $D/of_node/compatible 2>/dev/null | tr '\0' ' ')"
    for f in $D/of_node/*; do [ -f "$f" ] && echo "  of_node/${f##*/}: $(hexof $f | cut -c1-96)"; done
    sec "frame: size, contents as read (hex), twice; how long a read takes"
    echo "stat: $(stat -c '%s bytes, %A %U:%G' $F 2>&1)"
    hexof $F; echo; hexof $F; echo
    echo "as text:"; cat $F 2>&1 | cut -c1-400 | cat -v; echo
    now; t0=$NOW; i=0; while [ $i -lt 50 ]; do cat $F > /dev/null; i=$((i + 1)); done; now
    echo "50 reads (cat, one fork each): $(( (NOW - t0) * 10 )) ms"
    sec "led_current, boot_animation (read only)"
    for f in led_current boot_animation; do echo "$f: $(cat $D/$f 2>&1 | cut -c1-300 | cat -v)  [hex $(hexof $D/$f | cut -c1-80)]"; done
    sec "every other attribute of the driver (read only)"
    for f in $D/*; do
        case ${f##*/} in frame|led_current|boot_animation|uevent|modalias|name) continue;; esac
        [ -f "$f" ] && [ -r "$f" ] && echo "${f##*/}: $(cat $f 2>&1 | cut -c1-200 | tr '\n' ' ')"
    done
    sec "mute ring PWM (read only)"
    ls -laZ /sys/class/pwm/pwmchip0/ /sys/class/pwm/pwmchip0/pwm1/ 2>&1
    for f in period duty_cycle enable polarity; do echo "pwm1/$f: $(cat /sys/class/pwm/pwmchip0/pwm1/$f 2>&1)"; done
    echo "gpio-privacy: $(cat /sys/devices/platform/gpio-privacy/state 2>&1)"
    sec "kernel: the driver's functions and messages"
    grep -iE "frame|led_current|boot_anim" /proc/kallsyms 2>/dev/null | grep -iE "led|is31|lp50|lp55|aw20|frame_(show|store)" | head -40
    dmesg 2>/dev/null | grep -iE "led|is31|lp5|0-003f|pwm" | tail -30
    sec "who sets the files up (init and ueventd rc)"
    grep -nE "0-003f|led_current|boot_animation|/frame|pwmchip|ledcontroller" /*.rc /system/etc/init/*.rc /vendor/etc/init/*.rc \
        /system/vendor/etc/init/*.rc /ueventd*.rc 2>/dev/null
    sec "ledcontroller: process, label, open files, CPU and writes while idle"
    lc; [ -n "$LC" ] || die "ledcontroller not running"
    echo "pid $LC: $(cat /proc/$LC/cmdline | tr '\0' ' ')"; echo "label: $(cat /proc/$LC/attr/current)"
    grep -E "^(Uid|Gid|Groups|VmRSS|Threads)" /proc/$LC/status
    ls -l /proc/$LC/fd 2>&1 | grep -vE "socket:|pipe:|anon_inode|/dev/(null|binder|__properties__)"
    for fd in /proc/$LC/fd/*; do case $(readlink $fd) in *frame|*led_current|*pwm*) echo "$fd -> $(readlink $fd): $(cat /proc/$LC/fdinfo/${fd##*/} | tr '\n' ' ')";; esac; done
    echo "active patterns (ledctrl -g): $(pats_active)"; echo "brightness (ledctrl -l): $(ledctrl -l 2>&1 | tr '\n' ' ')"
    echo "persist.ledbrightness.bootup: $saved_level"
    ls -la /data/ledcontroller/ 2>&1; ls /system/lib/*led* /system/lib/hw/*led* /vendor/lib/*led* 2>/dev/null
    sec "frame sampled 10 s, nothing done (an idle Echo: please do not speak to it)"
    sample 10 idle
    sec "may hassmic's user write the frame?  (access() as the daemon's user and SELinux domain, nothing written)"
    H=$(pidof hassmic); H=${H%% *}
    [ -n "$H" ] && { echo "hassmic pid $H, label $(cat /proc/$H/attr/current)"; grep -E "^(Uid|Gid|Groups)" /proc/$H/status; }
    R= DAEMON_USER= DAEMON_GROUPS=
    for d in /data/local/hassmic/ota/current /system/hassmic /data/local/hassmic; do
        [ -x $d/runas ] && [ -f $d/device.conf ] || continue
        R=$d/runas; set -- $(. $d/device.conf; echo "$DAEMON_USER $DAEMON_GROUPS"); DAEMON_USER=$1 DAEMON_GROUPS=$2; break
    done
    if [ -n "$R" ] && [ -n "$DAEMON_USER" ]; then
        for f in $F $D/led_current /sys/class/pwm/pwmchip0/pwm1/duty_cycle; do
            $R -Z u:r:hassmic:s0 $DAEMON_USER $DAEMON_GROUPS /system/bin/sh -c "test -w $f" 2>/dev/null && w=yes || w=no
            $R -Z u:r:hassmic:s0 $DAEMON_USER $DAEMON_GROUPS /system/bin/sh -c "test -r $f" 2>/dev/null && r=yes || r=no
            echo "$f: hassmic reads $r, writes $w"
        done
    else echo "no runas or no device.conf found: not checked"; fi
    ;;

stock)
    idle_ok; lc
    trap 'unset_all; bright_restore; exit 1' INT TERM
    sec "stock patterns, the frame sampled while each shows (first lines of its file beside it)"
    for p in $SOLID $MOVING; do
        echo "--- $p:"; head -n 3 $LR/$p.animation | cut -c1-120
        play $p; sample 3 "$p"; unset_all; sleep 1
    done
    sec "brightness: the frame and led_current at each level, $SOLID showing"
    echo "before: $(ledctrl -l 2>&1 | tr '\n' ' '), persist $saved_level, led_current $(cat $D/led_current 2>&1)"
    play $SOLID; sleep 1
    for b in 100 50 25 10 1; do
        ledctrl -a off -b $b > /dev/null 2>&1
        echo "-- level $b"; sample 4 "level $b"; echo "  led_current: $(cat $D/led_current 2>&1)  pwm1 duty: $(cat /sys/class/pwm/pwmchip0/pwm1/duty_cycle 2>&1)"
    done
    unset_all; bright_restore; sleep 4
    echo "after: $(ledctrl -l 2>&1 | tr '\n' ' '), led_current $(cat $D/led_current 2>&1), frame $(hexof $F)"
    ;;

write)
    idle_ok; lc
    [ -n "$SOLID" ] || die "no stock solid pattern found"
    restore() {
        cat $B/dark.frame > $F 2>/dev/null
        unset_all; play $SOLID; sleep 0.3; unset_all; sleep 0.5     # ledcontroller writes the frame itself again
        bright_restore
        echo "restored: frame $(hexof $F)"
    }
    trap 'restore; exit 1' INT TERM
    cat $F > $B/dark.frame; echo "dark frame:  $(hexof $B/dark.frame)"
    # What ledcontroller writes for solid_blue (00F) at brightness 100 and what frame then reads back, per model as
    # measured; only that case is written here (the stock stage shows both at every level):
    #   donut: the HAL's pwm 0.993 makes 0000FD; the driver scales each channel by the unit's calibration as it takes a
    #          frame (0000A9 written read back 000071, blue x 0.668), so frame shows 0000A9 (2026-11-12)
    #   biscuit (and radar, the same HAL table "default"): pwm 0.875 makes 0000DF, and frame reads back 0000DF, no
    #          scaling seen (2026-11-13; at 50/25/10/1: C0, 68, 3C, 22 = the table's pwm x 255, truncated)
    case $(getprop ro.product.device) in
    donut*) IN=FD BACK=a9;;
    biscuit*|radar*) IN=DF BACK=df;;
    *) die "no measured case for $(getprop ro.product.device): run the stock stage and add one";;
    esac
    ledctrl -a off -b 100 > /dev/null 2>&1; sleep 1                   # bright_restore puts it back
    SOLID=solid_blue; [ -f $LR/$SOLID.animation ] || { restore; die "no solid_blue"; }
    play $SOLID; sleep 1.5; cat $F > $B/stock.frame; echo "stock frame ($SOLID, as read): $(cat $B/stock.frame)"
    want=; i=0; while [ $i -lt 12 ]; do want=${want}0000$BACK; i=$((i + 1)); done
    [ "$(tr 'A-F' 'a-f' < $B/stock.frame)" = "$want" ] || { restore; die "not stock's 0000$BACK on every LED at brightness 100: stopped"; }
    print -r -- "$(echo $want | sed "s/0000$BACK/0000$IN/g")" > $B/in.frame; echo "our input: $(cat $B/in.frame)"

    sec "1. stock's own input written while stock shows it: must read back as stock's frame (no change to see)"
    if ! err=$(cat $B/in.frame 2>&1 > $F); then echo "  refused: $err"; restore; die "the driver does not take it: stopped here"; fi
    back=$(hexof $F); echo "  read back: $(cat $F)"
    [ "$back" = "$(hexof $B/stock.frame)" ] || { restore; die "read back differs from stock's: stopped here"; }
    unset_all; sleep 1

    sec "2. that frame on the idle ring: does it stay?  (the ring shows $SOLID's colour for 5 s)"
    now; t0=$NOW; cat $B/in.frame > $F; now; echo "  one write: $(( (NOW - t0) * 10 )) ms (cat, fork included)"
    sample 5 "after our write, idle"
    cat $B/dark.frame > $F; sleep 0.5; echo "  dark again: $(hexof $F)"

    sec "3. over a static pattern ($SOLID): the dark frame written, does ledcontroller put its own back?"
    play $SOLID; sleep 1.5; cat $B/dark.frame > $F; sample 4 "dark written over $SOLID"; unset_all; sleep 1

    if [ -n "$MOVING" ]; then
        sec "4. over a moving pattern ($MOVING): our frame written, how soon is it overwritten?"
        play $MOVING; sleep 1.5; cat $B/in.frame > $F; sample 3 "ours over $MOVING"; unset_all; sleep 1
    fi

    sec "5. speed: 200 writes of the same frame ($SOLID's colour stays on, no flicker)"
    lc_cpu; c0=$CPU; sys_cpu; s0=$BUSY; t0=$TOT; now; n0=$NOW
    i=0; while [ $i -lt 200 ]; do cat $B/in.frame > $F; i=$((i + 1)); done
    now; sys_cpu; lc_cpu
    ms=$(( (NOW - n0) * 10 )); [ $ms -gt 0 ] || ms=1
    echo "  cat (a fork each): $ms ms, $(( 200000 / ms )) writes/s; all CPUs busy $(( (BUSY - s0) * 100 / (TOT - t0 + 1) )) %" \
         "of the time; ledcontroller $(( (CPU - c0) * 10 )) ms CPU"
    # the same bytes through the shell's builtin print (no fork): only if the shell can hold them exactly (text)
    v=$(cat $B/in.frame); want=$(hexof $B/in.frame); nl=
    print -rn -- "$v" > $B/t.frame; [ "$(hexof $B/t.frame)" = "$want" ] || { print -r -- "$v" > $B/t.frame; nl=1; }
    if [ "$(hexof $B/t.frame)" = "$want" ]; then
        sys_cpu; s0=$BUSY; t0=$TOT; now; n0=$NOW
        i=0; if [ -n "$nl" ]; then while [ $i -lt 200 ]; do print -r -- "$v" > $F; i=$((i + 1)); done
        else while [ $i -lt 200 ]; do print -rn -- "$v" > $F; i=$((i + 1)); done; fi
        now; sys_cpu; ms=$(( (NOW - n0) * 10 )); [ $ms -gt 0 ] || ms=1
        echo "  shell builtin (no fork): $ms ms, $(( 200000 / ms )) writes/s; all CPUs busy $(( (BUSY - s0) * 100 / (TOT - t0 + 1) )) %"
        echo "  read back: $(hexof $F)"
    else echo "  (the frame is not text the shell can hold: no builtin test)"; fi
    restore
    ;;
*) die "stage: read, stock or write";;
esac
echo; echo "led-probe $STAGE: done"
