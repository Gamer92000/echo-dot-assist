# Sourced by the guided setup and scripts/kiosk-install.sh, from the repository root: the Echo Shows' optional apps.
# Needs OUT (a folder for the downloads) and an Echo on adb.  sh, curl, sha256sum, python3 (all of them setup's anyway).

# Simple Keyboard: the image has no input method (Amazon's needs its account libraries), so an app that shows a text field
# gets no keyboard.  Open source, only the VIBRATE permission, no network: release 148 = 6.7, checked against its checksum.
KEYBOARD_URL=https://github.com/rkkr/simple-keyboard/releases/download/148/app-release.apk
KEYBOARD_SHA=b0a81d62e5229a28d90ff2c7a25a7dc434c56f23e4304158e1adb61192b9978a
KEYBOARD_IME=rkr.simplekeyboard.inputmethod/.latin.LatinIME
sha_is() { [ "$(sha256sum < "$1" | cut -c1-64)" = "$2" ]; }
keyboard_get() {
    f=$OUT/simple-keyboard-148.apk
    { [ -s "$f" ] && sha_is "$f" "$KEYBOARD_SHA"; } || curl -fL --retry 3 -o "$f" "$KEYBOARD_URL" || return 1
    sha_is "$f" "$KEYBOARD_SHA" || { rm -f "$f"; echo "Simple Keyboard: wrong checksum" >&2; return 1; }
    adb_install "$f" || return 1
    # athena: "Unknown id: null" right after the install, InputMethodManagerService has not seen the package yet
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do adb shell ime list -a | grep -qF "$KEYBOARD_IME" && break; sleep 1; done
    adb shell ime enable $KEYBOARD_IME > /dev/null && adb shell ime set $KEYBOARD_IME > /dev/null
}

# adb's own word, not its exit status: older ones exit 0 on "Failure [...]"
adb_install() {
    o=$(adb install -r "$1" 2>&1) || { echo "$o" >&2; return 1; }
    case "$o" in *Success*) ;; *) echo "$o" >&2; return 1;; esac
}

# Kiosk Satellite: the newest release, not a pinned one: the app updates itself from there as soon as it runs, so a pinned
# file would only mean a commit here per release of theirs.  GitHub's sha256 comes in the same answer as the address, so
# it catches a broken download and nothing more.  What is pinned is who signed it: KIOSK_CERT, the SHA-256 of the signing
# certificate, looked at on the PC before anything reaches the Echo (apkcert.py), since the Echo's kiosk_tick grants an
# app of that name its permissions and runs its script within 10 s of seeing it.
# 32-bit ARM on every Show (armeabi-v7a; docs/kiosk-satellite.md).
KIOSK_CERT=2a77134ce11f65ba9d08bed669fdcb742a25633c5665a6135aeca30040b068fc
KIOSK_RELEASES=${KIOSK_RELEASES:-https://api.github.com/repos/jxlarrea/kiosk-satellite/releases/latest}
# prints "<name> <sha256> <url>"
kiosk_latest() {
    curl -fsSL --retry 3 "$KIOSK_RELEASES" | python3 -I -c '
import json, re, sys
for a in json.load(sys.stdin.buffer).get("assets", []):
    n, d, u = a.get("name", ""), a.get("digest") or "", a.get("browser_download_url", "")
    if not n.endswith(".armeabi-v7a.apk"): continue
    if not re.fullmatch(r"kiosk-satellite-[0-9A-Za-z._+-]+\.apk", n): sys.exit("odd asset name: " + n[:80])
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", d): sys.exit(n + ": GitHub lists no sha256 for it")
    if not re.fullmatch(r"https://github\.com/jxlarrea/kiosk-satellite/releases/download/[0-9A-Za-z._+/-]+", u): sys.exit(n + ": unexpected address")
    print(n, d[7:], u); break
else: sys.exit("no armeabi-v7a APK in the newest release")'
}
kiosk_get() {
    r=$(kiosk_latest) || return 1
    [ -n "$r" ] || return 1
    set -- $r
    f=$OUT/$1
    { [ -s "$f" ] && sha_is "$f" "$2"; } || curl -fL --retry 3 -o "$f" "$3" || return 1
    sha_is "$f" "$2" || { rm -f "$f"; echo "$1: wrong checksum" >&2; return 1; }
    python3 -I scripts/lib/apkcert.py "$f" "$KIOSK_CERT" || {
        echo "If the app's developer changed keys on purpose, KIOSK_CERT in scripts/lib/kiosk.sh has to follow." >&2
        return 1
    }
    adb_install "$f"
}

# Taking it off again.  kiosk_grants made it a device admin, and Android refuses to uninstall an active admin
# (DELETE_FAILED_DEVICE_POLICY_MANAGER); `dpm remove-active-admin` is forceRemoveActiveAdmin, for testOnly admins only,
# root or not.  Disabling the admin's receiver does it: the device policy service looks at its admins on every
# PACKAGE_CHANGED and drops one whose receiver no longer resolves (handlePackagesChanged), and set-active-admin refuses a
# disabled receiver ("Unknown admin", findAdmin), so a kiosk_grants meanwhile cannot bring it back.  Read in the
# services.jar of checkers, cronos and crown; athena's is compact dex, AOSP 9's code is the same.  The admins as dumpsys
# lists them ("    <package>/<receiver>:"), not only the one kiosk_grants sets.
kiosk_admins() { adb shell dumpsys device_policy | tr -d '\r' | sed -n "s|^ *\($KIOSK_PACKAGE/[^ :]*\):\$|\1|p"; }
kiosk_admins_back() { for c in $1; do adb shell pm enable "$c" > /dev/null; done; }
kiosk_remove() {
    adb shell pm path $KIOSK_PACKAGE | grep -q '^package:' || { echo "Kiosk Satellite is not installed"; return 0; }
    a=$(kiosk_admins) off=
    for c in $a; do
        case "$(adb shell pm disable "$c" 2>&1)" in
        *"new state: disabled"*) off="$off $c";;
        *) kiosk_admins_back "$off"; echo "$c: Android did not disable it" >&2; return 1;;
        esac
    done
    # the broadcast goes out at once (pm disable kills the app); its handling takes a moment
    i=0
    while [ -n "$(kiosk_admins)" ]; do
        i=$((i + 1))
        [ $i -le 15 ] || {
            kiosk_admins_back "$off"
            echo "Android still lists it as a device admin; docs/kiosk-satellite.md \"Uninstalling\" has the way by hand" >&2
            return 1
        }
        sleep 1
    done
    o=$(adb shell pm uninstall $KIOSK_PACKAGE 2>&1) || :
    case "$o" in *Success*) ;; *) echo "$o" >&2; return 1;; esac
}
