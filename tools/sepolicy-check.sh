#!/bin/sh
# tools/sepolicy-check.sh [codename...]: hassmic's SELinux rules (scripts/system/sepolicy-hassmic.rules) compiled into
# each model's own policy the way the install does it, and what the result lets the plugins' domain do read back out
# of it (docs/plugins.md 2.7).  On the PC: magiskpolicy (Magisk v30.7's, pinned) under qemu against the unpacked
# firmware (tools/qrun.sh), as scripts/install-boot.sh runs it; install-system.sh runs the same steps on the Echo.
#   base: the policy as the model's install finds it.  The Shows (INSTALL=boot): their boot image's ramdisk, as
#   install-boot.sh takes it; the others: the firmware's own /sepolicy (athena: its vendor's precompiled one), which on
#   an Echo is the unlock's patch of it (permissive su and adbd, an init -> su rule): not on the PC, and nothing of
#   what is checked here depends on it but su being permissive, which every model's stock policy has already.
#   then: the model's devices/<codename>/sepolicy.rules, sepolicy-mirror.sh's rules from that, sepolicy-hassmic.rules
#   (the install's order), and the checks the install itself makes of the result.
# What it checks of the plugins' domain: every rule whose source is hassmic_plugin is one of the list below and every
# one of the list is there (nothing more through an attribute: the types carry only hassmic_plugin_type, which no
# other rule names); nothing makes it permissive; no other domain but su and hassmic reaches it (su: the launcher;
# hassmic: its `*`); hassmic cannot enter it or ptrace it; the relabel pair is the only relabel hassmic has.
# Not a device test: that a plugin runs in it on an Echo is not shown by this (docs/plugins.md 13).
# Exit 0 when every model checked passes; a model without its firmware or the tool is skipped and said.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
SUM=e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a     # Magisk v30.7's libmagiskpolicy.so (scripts/lib/setup.sh)
TOOL=
for t in firmware/*/magiskpolicy32 toolchain/magiskpolicy32; do
    [ -f "$t" ] && [ "$(sha256sum < "$t" | cut -c1-64)" = $SUM ] && { TOOL=$t; break; }
done
[ -n "$TOOL" ] || { echo "sepolicy-check: no magiskpolicy (Magisk v30.7's, scripts/lib/setup.sh magiskpolicy_get) under firmware/*/: nothing checked"; exit 1; }
OUT=build/sepolicy-check; mkdir -p $OUT
models=${*:-$(ls devices | while read -r d; do [ -f devices/$d/sepolicy.rules ] && echo $d; done)}

# What the plugins' domain may do, and nothing else (magiskpolicy --print-rules' own form: one class a line, its
# permissions sorted as the policy lists them, so compared as sets below)
WANT='allow hassmic_plugin hassmic_plugin_file file read open getattr map execute execute_no_trans
allow hassmic_plugin rootfs dir search
allow hassmic_plugin proc dir search
allow hassmic_plugin proc lnk_file read
allow hassmic_plugin hassmic_plugin dir search
allow hassmic_plugin hassmic_plugin lnk_file read
allow hassmic_plugin hassmic_plugin process fork sigkill sigstop signull signal getsched
allow hassmic_plugin su process sigchld
allow hassmic_plugin hassmic fd use
allow hassmic_plugin su fd use
allow hassmic_plugin hassmic unix_stream_socket read write getattr getopt setopt shutdown
allow hassmic_plugin hassmic tcp_socket read write getattr getopt setopt shutdown
allow hassmic_plugin hassmic udp_socket read write getattr getopt setopt shutdown
allow hassmic_plugin hassmic fifo_file write getattr
allow hassmic_plugin su fifo_file write getattr
allow hassmic_plugin hassmic_plugin fifo_file read write getattr
allow hassmic_plugin null_device chr_file read write getattr
allow hassmic_plugin system_data_file file read getattr'
# a rule's permissions as a sorted list: "allow s t c { a b }" and "allow s t c a" alike
norm() { sed -e 's/[{}]//g' -e 's/  */ /g' -e 's/ $//' | while read -r k s t c p; do echo "$k $s $t $c $(echo $p | tr ' ' '\n' | sort | tr '\n' ' ')"; done | sed 's/ $//' | sort; }

q() { DEVICE=$QDEV tools/qrun.sh -t 300 "$TOOL" "$@"; }
fails=0
for m in $models; do
    . devices/$m/device.conf 2>/dev/null || { echo "SKIP $m: no device.conf"; continue; }
    case "$INSTALL" in
    boot) base=firmware/$m/boot/rd/sepolicy;;
    running) base=firmware/$m/rootfs/system/vendor/etc/selinux/precompiled_sepolicy;;
    *) base=firmware/$m/rootfs/sepolicy;;
    esac
    # qemu needs a bionic linker and libc: the model's own rootfs if unpacked, else any other model's (the tool is static
    # apart from those)
    QDEV=$m; [ -e firmware/$m/rootfs/system/bin/linker ] || QDEV=$(ls -d firmware/*/rootfs/system/bin/linker 2>/dev/null | head -n1 | cut -d/ -f2)
    [ -f "$base" ] || { echo "SKIP $m: no $base (unpack the firmware: devices/$m/README.md)"; continue; }
    [ -n "$QDEV" ] || { echo "SKIP $m: no rootfs with a linker for qemu"; continue; }
    d=$OUT/$m; rm -rf $d; mkdir -p $d
    q --load "$base" --apply devices/$m/sepolicy.rules --save $d/sepolicy.mid > $d/mp.log 2>&1
    q --load $d/sepolicy.mid --print-rules > $d/mid.txt 2>/dev/null && sh scripts/system/sepolicy-mirror.sh $d/mid.txt > $d/mirror.rules &&
        q --load $d/sepolicy.mid --apply scripts/system/sepolicy-hassmic.rules --apply $d/mirror.rules --save $d/sepolicy.new >> $d/mp.log 2>&1
    q --load $d/sepolicy.new --print-rules > $d/new.txt 2>/dev/null
    bad=
    [ -s $d/sepolicy.new ] && [ -s $d/new.txt ] || bad="$bad; the patch made no policy ($d/mp.log)"
    # the install's own checks (install-system.sh, install-boot.sh), the new type's among them
    ! grep -q 'does not exist' $d/mp.log || bad="$bad; magiskpolicy dropped a rule: $(grep 'does not exist' $d/mp.log | sort -u | head -n3 | tr '\n' ' ')"
    grep -q '^allow init su process' $d/new.txt || bad="$bad; no init -> su"
    grep -q '^type hassmic ' $d/new.txt && ! grep -q '^allow hassmic [^ ]* property_service' $d/new.txt && ! grep -qx 'permissive hassmic' $d/new.txt || bad="$bad; hassmic's domain is not as the install wants it"
    grep -qx 'type hassmic_plugin { hassmic_plugin_type }' $d/new.txt || bad="$bad; hassmic_plugin is not a type of its own attribute alone: $(grep '^type hassmic_plugin ' $d/new.txt)"
    grep -qx 'type hassmic_plugin_file { hassmic_plugin_type }' $d/new.txt || bad="$bad; hassmic_plugin_file: $(grep '^type hassmic_plugin_file ' $d/new.txt)"
    ! grep -qE '^permissive hassmic_plugin$' $d/new.txt || bad="$bad; hassmic_plugin is permissive"
    # what it may do: exactly the list
    grep -E '^allow(xperm)? hassmic_plugin ' $d/new.txt | norm > $d/plugin.has
    echo "$WANT" | norm > $d/plugin.want
    extra=$(comm -23 $d/plugin.has $d/plugin.want); missing=$(comm -13 $d/plugin.has $d/plugin.want)
    [ -z "$extra" ] || bad="$bad; more than its list: $(echo "$extra" | head -n5 | tr '\n' '|')"
    [ -z "$missing" ] || bad="$bad; less than its list: $(echo "$missing" | head -n5 | tr '\n' '|')"
    ! grep -qE '^(allow|auditallow|allowxperm|type_transition|type_change|type_member) hassmic_plugin_type ' $d/new.txt || bad="$bad; a rule names the attribute"
    ! grep -qE '^type_transition [^ ]+ [^ ]+ process hassmic_plugin$' $d/new.txt || bad="$bad; something goes into it on exec"
    # who reaches it: su (the launcher) and hassmic (its *) only, and neither may do more than said.  A rule reaches it
    # through the attribute as well as by name (magiskpolicy prints hassmic's `*` as "allow hassmic hassmic_plugin_type
    # process ..."), and the program's type is part of it: each looked for by every name (a review of 3326cd4: by
    # hassmic_plugin alone neither check could fail)
    others=$(grep -E '^allow [^ ]+ (hassmic_plugin|hassmic_plugin_type|hassmic_plugin_file) ' $d/new.txt | awk '{print $2}' | sort -u | grep -vxE 'su|hassmic|hassmic_plugin' | head -n5 | tr '\n' ' ')
    [ -z "$others" ] || bad="$bad; other domains reach it: $others"
    ! grep -E '^allow hassmic (hassmic_plugin|hassmic_plugin_type|hassmic_plugin_file) process ' $d/new.txt | grep -qE ' (dyntransition|transition|setcurrent|ptrace)( |$)' || bad="$bad; hassmic may enter it or ptrace it"
    grep -qE '^allow su hassmic_plugin process .*dyntransition' $d/new.txt || bad="$bad; su cannot enter it"
    # hassmic's relabels: that pair and no other
    rl=$(grep -E '^allow hassmic [^ ]+ (file|dir|lnk_file|sock_file|fifo_file|chr_file|blk_file) ' $d/new.txt | grep -E ' relabel(from|to)( |$| })' | awk '{print $3 " " $4}' | sort -u | tr '\n' ' ')
    [ "$rl" = "hassmic_plugin_file file system_data_file file " ] || bad="$bad; hassmic's relabels are not that pair: $rl"
    grep -qE '^allow hassmic_plugin_file labeledfs filesystem (\{ )?associate( })?$' $d/new.txt || bad="$bad; the program's type cannot be on /data"
    if [ -z "$bad" ]; then
        echo "ok   $m ($INSTALL, $base): hassmic_plugin has its $(wc -l < $d/plugin.has) rules and nothing else; only su enters it"
    else
        echo "FAIL $m ($INSTALL, $base)${bad}"; fails=$((fails + 1))
    fi
done
echo "sepolicy-check: $([ $fails = 0 ] && echo "all ok" || echo "$fails FAILED") (what the domain may do, each model: $OUT/<model>/plugin.has)"
exit $((fails != 0))
