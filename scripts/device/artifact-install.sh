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
#   artifact-install.sh migrate STATE DATA
# Wake word sets installed by hand under the short name (models/echo-de) get the one scripts/artifacts.sh gives them
# (echo-de-DE), so that the same set has one name on every Echo (the settings page copies by name).  main.sh runs it at
# every satellite start, before hassmic.  The name is the wake word's id, so the saved choice (state/wake_word) follows.
# Only languages Amazon serves in one region (scripts/lib/artifacts.sh LOCALES): en, fr, es come in several, and the
# files do not say which, so those stay as they are.  A long name already there: the short one goes if it holds the
# same files, else both stay.
umask 022
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
        # echo into the file it is: it stays the daemon's
        [ "$(cat $W 2>/dev/null)" = "$old" ] && echo "$new" > $W && echo "models: the active wake word follows ($new)"
    done
    exit 0
fi
STATE=$1 DATA=$2
A=$STATE/artifacts
[ -f $A/request ] || exit 1
ok=0
: > $A/result.tmp

say() { echo "$1" >> $A/result.tmp; }
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

ids=$(cat $A/request); rm -f $A/request
for id in $ids; do install_one "$id"; done
[ -n "$DAEMON_USER" ] && chown $DAEMON_USER $A/result.tmp
mv $A/result.tmp $A/result
cat $A/result
[ $ok = 1 ]
