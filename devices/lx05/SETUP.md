# LX05: UART, overlay and local network

[По-русски](SETUP_RU.md) · [LX05 port](README.md)

These are notes from preparing **one LX05 running ROM 1.46.2**, not a tested fresh-device installer. Partition names, bootloader behaviour and init scripts may differ on other units. Changes to U-Boot or startup can make the speaker inaccessible. Keep UART connected, save private backups off-device, and try a one-time boot before making it persistent. The original speaker was checked after reboot; this written procedure has not been run end-to-end on a second device.

## Root through UART

No exploit was required on the tested unit: after startup the serial console displayed `Please press Enter to activate this console`. Enter opened a BusyBox `root@mico:/#` shell and `id` reported `uid=0`. Other firmware may restrict this console.

Open the speaker and solder to its UART **TX, RX and GND** pads after identifying them on your board. Their exact positions on this revision were not recorded here; do not guess the pinout. Connect speaker TX to USB–UART RX, speaker RX to adapter TX, and connect ground to ground. Measure and match the logic voltages first; do not connect the adapter's power wire to an independently powered speaker. Use **115200 baud, 8N1, no flow control**. Keep UART available for recovery if SSH or startup fails.

## Original layout and backups

On the tested unit the second root partition was `/dev/nand0p5` (read-only SquashFS), `/data` was ext4 on `/dev/nand0p9`, and U-Boot environment was `/dev/nand0p1`. First inspect `cat /proc/cmdline`, `mount`, `ls -l /dev/by-name` and `fw_printenv bootcmd boot_second setargs_second bootdelay`. Use these partition numbers only if your layout matches. The bootloader verified kernel and rootfs signatures; this setup did not rewrite SquashFS.

Save the complete text U-Boot environment and important data **to another computer** before changing anything. On the tested unit the first 128 KiB of the environment partition were additionally saved with `dd if=/dev/nand0p1 of=/data/lx05/env-before.bin bs=131072 count=1` and copied off-device. Environment and backups can contain secrets and identifiers: keep them private and do not blindly write the raw backup to flash. The original `bootcmd` was `run setargs_second boot_second`.

## Prepare the overlay and network guard

The example uses LAN `10.42.0.0/24` and interface `wlan0`. **Replace the subnet with your own** throughout, including `LX05_LAN_PREFIX` in the [README](README.md). The working configuration used a single IPv4 /24 and DHCP. Keep UART available and block the speaker's internet access at the router before making changes.

Create the directories on the running system. Overlay `upper` and `work` must be on the same filesystem, and `work` must initially be empty:

```sh
mkdir -p /data/lx05/upper/init.d /data/lx05/upper/dropbear /data/lx05/upper/config /data/lx05/work
chmod 700 /data/lx05
cp /etc/init.d/wireless /data/lx05/upper/init.d/wireless
cp /etc/init.d/dropbear /data/lx05/upper/init.d/dropbear
cp /etc/config/dropbear /data/lx05/upper/config/dropbear
cp /bin/simple_dhcp.sh /data/lx05/dhcp.sh
```

Create a private `/data/lx05/wpa.conf` for your network in `wpa_supplicant` format, mode `600`. The tested network used WPA-PSK; an example structure follows (replace the uppercase fields with your own):

```text
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1
network={
    ssid="YOUR_SSID"
    psk=YOUR_64_DIGIT_HEX_PSK
    key_mgmt=WPA-PSK
    scan_ssid=1
}
```

The 64-digit PSK can be generated on your computer with `wpa_passphrase` for your SSID and password; keep the resulting file private. Before replacing `/data/wifi/wpa_supplicant.conf`, save its original separately: on the tested unit it became a symlink to `/tmp/lx05-wpa.conf`, which can be dangling during stock boot without the guard.

In the copied `/data/lx05/dhcp.sh`, immediately after `/sbin/ifconfig $interface $ip $BROADCAST $NETMASK` inside `setup_interface()`, add:

```sh
    ip route replace 10.42.0.0/24 dev wlan0 table 123
    ip route replace 255.255.255.255/32 dev wlan0 table 123
    ip route replace 224.0.0.251/32 dev wlan0 table 123
```

These routes allow the LAN, DHCP broadcast and mDNS respectively. The tested copy of `/data/lx05/upper/init.d/wireless` also added the first two routes immediately before `/etc/init.d/dhcpc restart`; at the start of `start_service()` it ran `/data/lx05/network-guard || return 1`. Apply the changes to the copies, not to SquashFS, and verify insertion points and shell syntax. Script layouts are firmware-specific.

Create `/data/lx05/network-guard`:

```sh
#!/bin/sh
ip route replace unreachable default table 123 || exit 1
ip rule show | grep -q '100:.*lookup 123' || ip rule add pref 100 lookup 123 || exit 1
echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6 || exit 1
echo 1 > /proc/sys/net/ipv6/conf/default/disable_ipv6 || exit 1
cp /data/lx05/wpa.conf /tmp/lx05-wpa.conf || exit 1
chmod 600 /tmp/lx05-wpa.conf
```

Create `/data/lx05/early.sh`:

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
ip route add unreachable default table 123 || exec /bin/sh
ip rule add pref 100 lookup 123 || exec /bin/sh
echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6 || exec /bin/sh
echo 1 > /proc/sys/net/ipv6/conf/default/disable_ipv6 || exec /bin/sh
mount -t overlay overlay -o lowerdir=/etc,upperdir=/data/lx05/upper,workdir=/data/lx05/work /etc || exec /bin/sh
mount --bind /data/lx05/dhcp.sh /bin/simple_dhcp.sh || exec /bin/sh
echo LX05_EARLY_READY
exec /sbin/init
```

This is the working unit's script: failures of critical operations leave it in a UART shell. Run `chmod 700 /data/lx05/early.sh /data/lx05/network-guard`, and `sh -n` on both scripts and the copied `wireless` and `dhcp.sh`. Only after backing up the original configuration, replace it with a symlink and run `sync`:

```sh
# Once only, if /data/lx05/wpa.original does not already exist:
mv /data/wifi/wpa_supplicant.conf /data/lx05/wpa.original
ln -s /tmp/lx05-wpa.conf /data/wifi/wpa_supplicant.conf
sync
```

The guard creates the temporary file before Wi-Fi starts. Do not repeat `mv` over your backup.

For SSH, in the copy `/data/lx05/upper/init.d/dropbear` we changed the release-channel restriction in `start_service()` to `if false; then`, keeping its `return 0` inside that block. Inspect your script first: do not replace unrelated conditions. In `/data/lx05/upper/config/dropbear` we set `option PasswordAuth '0'`, `option RootPasswordAuth '0'`, port 22 and the persistent server key path `/data/etc/dropbear/dropbear_rsa_host_key`. Put your **public** key in `/data/lx05/upper/dropbear/authorized_keys`, mode `600`. Never publish the private key. Older Dropbear may require compatible SSH algorithms; verify the server host key and key-based access during the one-time boot before changing `bootcmd`.

## Test once, then make boot persistent

The tested unit used `fw_setenv bootdelay 5` to give time to stop U-Boot. First verify the value and ability to interrupt boot on UART. The following file gives `fw_setenv` the `lxargs` value without Linux shell expansion of `${partitions}`. Use it **only with matching partitions and U-Boot commands**:

```sh
cat > /data/lx05/uboot-lxargs.txt <<'EOF'
lxargs setenv bootargs console=ttyS0,115200 root=/dev/nand0p5 rootwait init=/bin/sh partitions=${partitions} gpt=1 -- -c '"mount -text4 /dev/nand0p9 /data;exec /data/lx05/early.sh"'
EOF
fw_setenv -s /data/lx05/uboot-lxargs.txt
fw_printenv lxargs bootcmd bootdelay
sync
```

Reboot and interrupt U-Boot. Run `run lxargs`, then `printenv bootargs`: check that `partitions` expanded and the `-c` argument contains the `/data` mount. Then run `run boot_second`. The spelling `mount -text4` without a space matters: `-t ext4` caused `Too many args (max. 16)` on this bootloader. Watch UART for `LX05_EARLY_READY`, wait for startup, and verify overlay, networking and SSH. During this test `bootcmd` remains stock.

Only after the test succeeded, the working unit's `bootcmd` was changed from Linux:

```sh
fw_setenv bootcmd 'run lxargs boot_second'
fw_printenv bootcmd lxargs bootdelay
sync
```

Reboot and repeat the checks. Do not change unrelated environment values.

## Check isolation and recover

```sh
mount | grep 'overlay on /etc'
ip rule show
ip route show table 123
ip route get 10.42.0.1          # replace with a host on your LAN
ip route get 1.1.1.1            # expected: Host is unreachable
cat /proc/sys/net/ipv6/conf/all/disable_ipv6
cat /proc/sys/net/ipv6/conf/default/disable_ipv6
```

Expect priority 100 pointing to table 123, `unreachable default`, LAN, DHCP broadcast and mDNS routes, working SSH, and IPv6 values of `1`. Also check both IPv4 and IPv6 egress at the router, including after reboots and Wi-Fi restarts. This is **policy routing, not a full firewall**: local hosts, DNS servers and proxies can still forward traffic, and other interfaces and failures must be considered separately. One `ip route get` test does not establish that OTA or voice data cannot escape.

For a one-time stock boot on the tested unit, interrupt U-Boot and run `run setargs_second boot_second`. For a persistent return we used U-Boot `setenv bootcmd 'run setargs_second boot_second'`, `saveenv`, then `boot`. **Stock boot does not apply this network guard**, so isolate the speaker from the internet first. The temporary Wi-Fi symlink may also be broken; restore the original file from your backup. Do not write the raw environment backup back to flash without separately verifying the procedure.
