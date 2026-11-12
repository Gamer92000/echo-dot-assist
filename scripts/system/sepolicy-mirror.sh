#!/system/bin/sh
# sepolicy-mirror.sh RULES: the rules that make hassmic's domain (sepolicy-hassmic.rules) what su was to everyone
# else, from RULES = magiskpolicy --print-rules of the policy patched so far (the model's sepolicy.rules applied: some
# add rules for su).  Every policy has its own: what other domains may do to su (binder calls back, fds, sockets,
# servicemanager, keystore; Amazon's own), su's attributes, and the domains su goes into on exec (amzn_dha's helper on
# the Dots), into which hassmic goes too unless that domain is permissive.  Runs on the Echo (install-system.sh:
# toybox sed, no awk) and on the PC (install-boot.sh).
r=$1
[ -s "$r" ] || { echo "sepolicy-mirror.sh: no rules in $r" >&2; exit 1; }
perm=" $(sed -n 's/^permissive //p' "$r" | tr '\n' ' ')"
sed -n 's/^type su { \(.*\) }$/typeattribute hassmic { \1 }/p' "$r"
# (magiskpolicy reads no "-" in a name: Amazon's neo-service-coordinator, which may read su's /proc files, may not ours)
sed -n 's/^\(allow[a-z]*\) \([^ ]*\) su /\1 \2 hassmic /p' "$r" | grep -v '^allow[a-z]* su ' | grep -v '^[^ ]* [^ ]*-'
sed -n 's/^type_transition su \([^ ]*\) process \([^ ]*\)$/\1 \2/p' "$r" | while read -r x d; do
    case "$perm " in *" $d "*) continue;; esac
    echo "type_transition hassmic $x process $d"
    echo "allow hassmic $d process { transition siginh rlimitinh noatsecure }"
done
