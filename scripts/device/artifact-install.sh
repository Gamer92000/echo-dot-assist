#!/system/bin/sh
# Root side of copying artifacts between Echos (src/hassmic/artifacts.c): hassmic, as the daemon's user, received an
# artifact through the settings page, checked its digest (and, a wake word set, that this Echo's engine loads it), staged
# it in state/artifacts/<stage>/ and named it in state/artifacts/request.  The models folders are root's (adb wrote them),
# so root puts it there:
#   artifact-install.sh STATE DATA        e.g. /data/local/hassmic/state /data/local/hassmic
# Ids: wake:<name> -> DATA/models/<name>, sound -> DATA/aed, whisper -> DATA/whisper.  Staged files are read as the daemon's
# user (READ_AS, main.sh: runas): whatever the daemon left there, a link included, only reaches what it may read anyway,
# never a file of root's.  Root only looks at names and types, then copies into a fresh folder of its own and swaps it in.
# Writes STATE/artifacts/result ("OK <id>" / "FAILED <id>: why" per line, owned by DAEMON_USER) and prints it; exit 0 if
# at least one went in (main.sh then restarts hassmic: the wake word list and the whisper model are read at start).
# Root writes nothing through a name in the daemon's folder: the request is renamed into DATA (root's) and read there
# as the daemon's user, the result made and chowned in DATA and renamed into STATE/artifacts.  A link the daemon left
# at either name (request -> a root file, result.tmp -> hassmic.conf) had root show it that file's words, or write the
# file and chown it to the daemon, which could then put commands in it for root's next start.
#   artifact-install.sh migrate STATE DATA
# Wake word sets installed by hand under the short name (models/echo-de) get the one scripts/artifacts.sh gives them
# (echo-de-DE), so that the same set has one name on every Echo (the settings page copies by name).  main.sh runs it at
# every satellite start, before hassmic.  The name is the wake word's id, so the saved choice (state/wake_word) follows.
# Only languages Amazon serves in one region (scripts/lib/artifacts.sh LOCALES): en, fr, es come in several, and the
# files do not say which, so those stay as they are.  A long name already there: the short one goes if it holds the
# same files, else both stay.
#   artifact-install.sh delete STATE DATA
# A wake word trained on this Echo (src/hassmic/enroll.c) that the owner removes on the settings page: hassmic names it in
# STATE/artifacts/delete ("wake:<name>").  Only a set that has the mark file enrolled.txt goes (a plain file: never Amazon's
# sets, which have none) and never the one STATE/wake_word names; main.sh then restarts hassmic, as for an install.
umask 022
# READ_AS: how root reads what the daemon left (main.sh: runas as its user).  Empty only on the PC (tests: the test's
# own user); not set at all would have root read the daemon's files itself
[ -n "${READ_AS+set}" ] || { echo "FAILED: READ_AS not set (main.sh gives it)"; exit 1; }
# Root-owned parent and a persistent inode: the daemon opens this read-only to fence recovery against a live installer.
lockdata=$2
case $1 in migrate|delete) lockdata=$3;; esac
# Android mksh closes nonstandard descriptors on exec; stdin keeps this lock inherited by copying children.
exec 0>>"$lockdata/.artifact-lock" || exit 1
chmod 644 "$lockdata/.artifact-lock"
flock -x 0 || exit 1
# folders $1 and $2 hold the same files, folders down included (toybox here has no diff -r)
same_tree() {
    for f in $1/* $2/*; do
        b=${f##*/}
        if [ -d "$1/$b" ] || [ -d "$2/$b" ]; then
            [ -d "$1/$b" ] && [ -d "$2/$b" ] && same_tree "$1/$b" "$2/$b" || return 1
        else cmp -s "$1/$b" "$2/$b" || return 1
        fi
    done
}
if [ "$1" = migrate ]; then
    STATE=$2 M=$3/models W=$2/wake_word
    for d in $M/*-de $M/*-it $M/*-ja $M/*-pt; do
        [ -d "$d" ] && [ ! -L "$d" ] || continue
        case ${d##*-} in de) r=DE;; it) r=IT;; ja) r=JP;; pt) r=BR;; esac
        old=${d##*/} new=${d##*/}-$r
        if [ -e $M/$new ]; then
            if same_tree "$d" "$M/$new"; then rm -rf "$d"; echo "models: $old removed, the same set is there as $new"
            else echo "models: $old and $new differ, both kept"; continue
            fi
        elif mv "$d" "$M/$new"; then echo "models: $old renamed $new"
        else continue
        fi
        # written as the daemon's user, as the file it is: it stays the daemon's, and a link only reaches what it may
        # write anyway
        [ "$($READ_AS cat $W 2>/dev/null)" = "$old" ] && echo "$new" | $READ_AS sh -c 'cat > "$1"' sh $W &&
            echo "models: the active wake word follows ($new)"
    done
    exit 0
fi
mode=install request=request
if [ "$1" = delete ]; then mode=delete request=delete; shift; fi
STATE=$1 DATA=$(cd -P "$2" && pwd -P) || exit 1
A=$STATE/artifacts
REQ=$DATA/.artifact-request RES=$DATA/.artifact-result
[ -f $A/$request ] || exit 1
# The folder itself is the daemon's to replace (STATE is its own): a link there had root remove, read and leave files in
# a folder of root's.  So into it (cd -P), the folder it is in checked to be STATE/artifacts itself (pwd -P: the folder,
# whatever names point at it now), and only names relative to it from here on.  STATE's own parent is root's.
real=$(cd -P "$STATE" && pwd -P)/artifacts
if ! cd -P $A 2>/dev/null || [ "$(pwd -P)" != "$real" ]; then
    cd /; rm -f $A 2>/dev/null                      # a link goes (rm without -r never follows one); a folder stays
    echo "FAILED: $A is not hassmic's own folder (a link?)"; exit 1
fi
A=.
rm -rf $REQ $RES
# a rename moves a link as a link; one name only (/data has no protected_hardlinks: a hard link to a file of root's)
mv $A/$request $REQ && [ -f $REQ ] && [ ! -L $REQ ] && set -- $(ls -ld $REQ 2>/dev/null) && [ "$2" = 1 ] ||
    { rm -rf $REQ; exit 1; }
ok=0
: > $RES

say() { echo "$1" >> $RES; }
good() { case $1 in ""|[!A-Za-z0-9]*|*[!A-Za-z0-9._-]*) return 1;; esac; [ ${#1} -le 63 ]; }

# the staged folder $1 into root's $2: plain files, and folders of them $3 levels down, as artifacts.c stages them
# (whisper_components/, BDPGeneratedFiles/, nttfusionconfig/ntt_conv/).  A link, a deeper folder or a bad name: 1, $why says.
# Directories are made here, root's; files are read as the daemon's user, so a link swapped in meanwhile gains nothing.
copy_in() {
    for f in $1/* $1/.[!.]* $1/..?*; do
        [ -e "$f" ] || [ -L "$f" ] || continue                  # the patterns that matched nothing
        b=${f##*/}
        if ! good "$b" || [ -L "$f" ]; then why="$b is not a plain file"; return 1; fi
        if [ -d "$f" ] && [ "$3" -gt 0 ]; then
            mkdir "$2/$b" && chmod 755 "$2/$b" || { why="cannot write $2/$b"; return 1; }
            copy_in "$f" "$2/$b" $(($3 - 1)) || return 1
        elif [ -f "$f" ]; then
            $READ_AS cat "$f" > "$2/$b" || { why="cannot copy $b (space?)"; return 1; }
            chmod 644 "$2/$b"
            n=$((n + 1))
        else why="$b is not a plain file"; return 1
        fi
    done
}

install_one() {
    id=$1
    case $id in
    wake:*) name=${id#wake:}; good "$name" || { say "FAILED $id: bad name"; return; }
            stage=$A/wake.$name; mkdir -p $DATA/models; chmod 755 $DATA/models; dest=$DATA/models/$name; tmp=$DATA/models/.new-$name;;
    sound) stage=$A/sound; dest=$DATA/aed; tmp=$DATA/.new-aed;;
    whisper) stage=$A/whisper; dest=$DATA/whisper; tmp=$DATA/.new-whisper;;
    *) say "FAILED $id: not an artifact"; return;;
    esac
    [ -f $stage.ready ] && [ -d $stage ] && [ ! -L $stage ] || { say "FAILED $id: not staged"; return; }
    rm -rf $tmp; mkdir $tmp || { say "FAILED $id: cannot write $tmp"; return; }
    n=0
    copy_in $stage $tmp 2 || { rm -rf $tmp; say "FAILED $id: $why"; return; }
    [ $n -gt 0 ] || { rm -rf $tmp; say "FAILED $id: empty"; return; }
    chmod 755 $tmp
    old=${tmp%/*}/.old-${dest##*/}
    rm -rf $old
    [ -e $dest ] && mv $dest $old
    mv $tmp $dest || { [ -e $old ] && mv $old $dest; say "FAILED $id: cannot put it in place"; return; }
    rm -rf $old $stage $stage.expect $stage.ready
    say "OK $id"; ok=1
}

delete_one() {
    id=$1
    case $id in wake:*) name=${id#wake:};; *) say "FAILED $id: not a wake word set"; return;; esac
    good "$name" || { say "FAILED $id: bad name"; return; }
    dest=$DATA/models/$name
    if [ ! -d "$dest" ] || [ -L "$dest" ] || [ ! -f "$dest/enrolled.txt" ] || [ -L "$dest/enrolled.txt" ]; then
        say "FAILED $id: not a wake word trained on this Echo"; return
    fi
    # The model tree is root's; read the daemon-owned selection only as the daemon.
    if $READ_AS grep -Fxq "$name" "$STATE/wake_word" 2>/dev/null; then
        say "FAILED $id: it is the wake word in use"; return
    fi
    rm -rf "$dest" && { say "OK deleted $id"; ok=1; } || say "FAILED $id: cannot remove it"
}

ids=$($READ_AS dd if=$REQ bs=4096 count=1 2>/dev/null); rm -f $REQ     # as the daemon: a hard link reads only what it may
for id in $ids; do "${mode}_one" "$id"; done
cat $RES
[ -n "$DAEMON_USER" ] && chown $DAEMON_USER $RES
chmod 644 $RES; rm -rf $A/result; mv -f $RES $A/result      # a folder by that name too (rm never follows a link)
[ $ok = 1 ]
