#!/system/bin/sh
# Apps of the owner's on an Echo with Android (the Shows), from the settings page or the Echo Local integration
# (src/hassmic/apps.c): an APK hassmic staged is installed, or an app of the owner's removed, by Android's pm.
# Root's: main.sh's ota_watch runs "apps.sh take" in the background when state/apps-request is there (pm takes 5 to 30
# s), one at a time (the lock).  The answer goes to /data/local/hassmic/apps/result, root's folder, readable by all:
#   <id> ok <package>        installed (or updated), or removed
#   <id> failed <why>        pm's reason (INSTALL_FAILED_...), or ours
# What may be removed is the same as what lockdown.sh lets out: an app in /data/app, no update of a system app, a uid of
# its own; never the screen app (state/kiosk's "app").  An install may update such an app only (pm itself refuses another
# signature); an update of Amazon's system apps cannot pass pm's signature check, and one that did would still be a
# system app's update: never let out by the firewall.
# The staged APK is the daemon's file: read as the daemon's user (READ_AS, runas, as artifact-install.sh does), so that
# no link in hassmic's folder can point root at anything.
# (tests: APPS_STATE, APPS_DIR, PACKAGES_XML, and pm first in PATH)
ST=${APPS_STATE:-/data/local/hassmic/state}
A=${APPS_DIR:-/data/local/hassmic/apps}
X=${PACKAGES_XML:-/data/system/packages.xml}
[ "$1" = take ] || { echo "usage: apps.sh take"; exit 2; }
mkdir -p $A && chmod 755 $A
mkdir $A/lock 2>/dev/null || exit 0
trap 'rm -rf $A/lock $A/req $A/new.apk $A/before' EXIT
rm -f $A/req
mv $ST/apps-request $A/req 2>/dev/null && [ -f $A/req ] && [ ! -L $A/req ] || exit 0
set -- $(dd if=$A/req bs=200 count=1 2>/dev/null | tr -cd 'a-zA-Z0-9_. \n')
id=$1 what=$2 arg=$3
case "$id" in ''|*[!0-9]*) echo "apps: request without an id, ignored"; exit 0;; esac
answer() { echo "$id $*" > $A/result.new && chmod 644 $A/result.new && mv $A/result.new $A/result; echo "apps: $what: $*"; }

# 0: $1 is an app of the owner's (lockdown.sh app_uids' rule)
owner() {
    case "$1" in ''|.*|*.|*..*|*[!A-Za-z0-9_.]*) return 1;; esac
    l=$(grep -m1 -F "<package name=\"$1\" " $X) || return 1
    case "$l" in *' codePath="/data/app/'*) ;; *) return 1;; esac
    case "$l" in *' sharedUserId='*) return 1;; esac
    f=${l#*' publicFlags="'}; f=${f%%'"'*}
    case "$f" in ''|*[!0-9-]*) return 1;; esac
    [ $((f & 1)) = 0 ]
}
# "<name> <update time>" of every package: what changed is what pm installed
stamps() { sed -n 's/^ *<package name="\([A-Za-z0-9_.]*\)" .* ut="\([0-9a-f]*\)".*/\1 \2/p' $X; }
screen_app() { head -1 $ST/screen_home 2>/dev/null; }   # the home app (screen.c): never removed from here

case "$what" in
install)
    case "$arg" in ''|*[!0-9]*) answer failed "no size"; exit 0;; esac
    [ -n "$READ_AS" ] || { answer failed "no READ_AS"; exit 0; }
    $READ_AS cat $ST/apps-staged.apk > $A/new.apk 2>/dev/null
    [ "$(wc -c < $A/new.apk)" = "$arg" ] || { answer failed "the staged APK is not $arg bytes"; exit 0; }
    chmod 644 $A/new.apk
    stamps > $A/before
    out=$(pm install -r $A/new.apk 2>&1)
    case "$out" in
    *Success*)
        rm -f $ST/apps-staged.apk
        p=$(stamps | grep -vxF -f $A/before | head -1); p=${p%% *}
        if [ -n "$p" ] && owner "$p"; then answer ok "$p"
        elif [ -n "$p" ]; then answer failed "$p is not an app of the owner's"
        else answer ok "-"; fi;;
    *) why=$(echo "$out" | sed -n 's/.*\[\([A-Z_]*\).*/\1/p' | head -1); answer failed "${why:-$(echo $out | tr -cd 'a-zA-Z0-9_ .:' | cut -c1-120)}";;
    esac;;
uninstall)
    owner "$arg" || { answer failed "$arg is not an app of the owner's"; exit 0; }
    [ "$arg" = "$(screen_app)" ] && { answer failed "$arg is the screen app"; exit 0; }
    out=$(pm uninstall "$arg" 2>&1)
    case "$out" in *Success*) answer ok "$arg";; *) answer failed "$(echo $out | tr -cd 'a-zA-Z0-9_ .:[]' | cut -c1-120)";; esac;;
*) answer failed "unknown request";;
esac
exit 0
