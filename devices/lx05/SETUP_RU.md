# LX05: UART, overlay и локальная сеть

[English](SETUP.md) · [Порт LX05](README_RU.md)

Это запись того, как была подготовлена **одна LX05 с ROM 1.46.2**. Это не проверенный установщик: расположение разделов, имена сценариев и поведение загрузчика на других ревизиях могут отличаться. Команды, меняющие среду U-Boot или загрузку, способны лишить устройство доступа. Держите подключённым UART, сохраните резервные копии вне колонки и сначала попробуйте одноразовую загрузку; пример ниже требует ручной адаптации. Рабочая колонка проверена после перезагрузки, но эта инструкция на второй колонке целиком не проходилась.

## Root через UART

На проверенной колонке не понадобился эксплойт: после загрузки на последовательной консоли появлялось `Please press Enter to activate this console`, и Enter сразу открывал BusyBox `root@mico:/#` с `uid=0`. Это наблюдение для ROM 1.46.2; на другой прошивке вход может быть закрыт.

Разберите колонку и припаяйтесь к контактным площадкам UART **TX, RX и GND**, предварительно определив их на своей плате. Точное расположение площадок для этой ревизии здесь не зафиксировано — не угадывайте распиновку. Подключите TX колонки к RX USB–UART, RX колонки к TX адаптера и соедините земли. Используйте согласованные логические уровни, предварительно измерив их; не подключайте провод питания адаптера к запитанной колонке. Терминал: **115200 бод, 8N1, без управления потоком**. Запустите колонку, нажмите Enter и проверьте `id`. UART понадобится и для восстановления, если SSH или загрузка не поднимется.

## Исходная схема и резервное копирование

На этом экземпляре вторая копия корня находилась на `/dev/nand0p5` (SquashFS, read-only), `/data` — ext4 на `/dev/nand0p9`, а U-Boot environment — на `/dev/nand0p1`. Проверьте на своём устройстве `cat /proc/cmdline`, `mount`, `ls -l /dev/by-name` и `fw_printenv bootcmd boot_second setargs_second bootdelay` **до изменений**. Приведённые номера разделов применимы только при совпадающей разметке. Подписи ядра и rootfs проверялись при загрузке; системный SquashFS здесь не переписывался.

Скопируйте полную текстовую среду U-Boot и резервные копии важных данных **на отдельный компьютер**. На проверенном экземпляре также сохранили первые 128 КиБ раздела environment (`dd if=/dev/nand0p1 of=/data/lx05/env-before.bin bs=131072 count=1`) и забрали копию с колонки. Environment и данные могут содержать идентификаторы и секреты: не публикуйте их и не записывайте образ обратно вслепую. Исходный `bootcmd` был `run setargs_second boot_second`.

## Подготовка overlay и сетевого guard

Пример использует LAN `10.42.0.0/24` и интерфейс `wlan0` — **замените сеть на свою** во всех показанных местах, включая `LX05_LAN_PREFIX` из [README](README_RU.md). Схема проверялась только для одной IPv4-подсети /24 и устройства, получающего адрес по DHCP. Перед изменениями обеспечьте доступ через UART и ограничьте доступ колонки в интернет на маршрутизаторе.

На работающей системе создайте каталоги; `upper` и `work` должны быть на одной файловой системе, а `work` изначально пустым:

```sh
mkdir -p /data/lx05/upper/init.d /data/lx05/upper/dropbear /data/lx05/upper/config /data/lx05/work
chmod 700 /data/lx05
cp /etc/init.d/wireless /data/lx05/upper/init.d/wireless
cp /etc/init.d/dropbear /data/lx05/upper/init.d/dropbear
cp /etc/config/dropbear /data/lx05/upper/config/dropbear
cp /bin/simple_dhcp.sh /data/lx05/dhcp.sh
```

Настройте `/data/lx05/wpa.conf` под свою сеть в формате `wpa_supplicant`, права `600`. На проверенном устройстве использовался WPA-PSK; пример структуры (вместо заглавных полей подставьте свои данные):

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

64-значный PSK можно сгенерировать на своём компьютере командой `wpa_passphrase` для своего SSID и пароля; готовый файл храните приватно. Не публикуйте конфигурацию и ключи. Перед заменой отдельно сохраните исходный `/data/wifi/wpa_supplicant.conf`: он был заменён ссылкой на `/tmp/lx05-wpa.conf`, которая без guard при штатной загрузке может оказаться нерабочей.

В скопированном `/data/lx05/dhcp.sh`, сразу после строки `/sbin/ifconfig $interface $ip $BROADCAST $NETMASK` внутри `setup_interface()`, добавьте:

```sh
    ip route replace 10.42.0.0/24 dev wlan0 table 123
    ip route replace 255.255.255.255/32 dev wlan0 table 123
    ip route replace 224.0.0.251/32 dev wlan0 table 123
```

Первый маршрут разрешает локальную сеть, второй — DHCP broadcast, третий — mDNS. Проверенная копия `/data/lx05/upper/init.d/wireless` также добавляла первые два маршрута непосредственно перед `/etc/init.d/dhcpc restart`; в начале `start_service()` она вызывала `/data/lx05/network-guard || return 1`. Внесите эти изменения в копию, не в исходный SquashFS, и проверьте `sh -n` и место вставки. Точная структура сценария зависит от версии прошивки.

Создайте `/data/lx05/network-guard`:

```sh
#!/bin/sh
ip route replace unreachable default table 123 || exit 1
ip rule show | grep -q '100:.*lookup 123' || ip rule add pref 100 lookup 123 || exit 1
echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6 || exit 1
echo 1 > /proc/sys/net/ipv6/conf/default/disable_ipv6 || exit 1
cp /data/lx05/wpa.conf /tmp/lx05-wpa.conf || exit 1
chmod 600 /tmp/lx05-wpa.conf
```

Создайте `/data/lx05/early.sh`:

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

Это содержимое рабочего экземпляра: при сбое критических операций он оставался в UART shell. Выполните `chmod 700 /data/lx05/early.sh /data/lx05/network-guard` и `sh -n` для обоих файлов, а также для копий `wireless` и `dhcp.sh`. Только после сохранения исходной конфигурации замените её ссылкой и выполните `sync`:

```sh
# Однократно, только если /data/lx05/wpa.original ещё не существует:
mv /data/wifi/wpa_supplicant.conf /data/lx05/wpa.original
ln -s /tmp/lx05-wpa.conf /data/wifi/wpa_supplicant.conf
sync
```

Guard создаёт временный файл перед запуском Wi-Fi. Не повторяйте `mv` поверх своей резервной копии.

Для SSH на нашем экземпляре в копии `/data/lx05/upper/init.d/dropbear` изменили условие в `start_service()`, которое запрещало запуск при release-канале, на `if false; then` — соответствующий `return 0` остался внутри блока. Проверьте точную структуру своего скрипта: не заменяйте другие условия. В `/data/lx05/upper/config/dropbear` установили `option PasswordAuth '0'`, `option RootPasswordAuth '0'`, порт 22 и путь к постоянному ключу сервера `/data/etc/dropbear/dropbear_rsa_host_key`. Собственный **открытый** ключ добавили в `/data/lx05/upper/dropbear/authorized_keys`, режим файла `600`. Не публикуйте приватный ключ. Старому клиенту/серверу могут понадобиться совместимые алгоритмы SSH; проверяйте ключ сервера и доступ по ключу во время пробной загрузки до изменения `bootcmd`.

## Одноразовый запуск и сохранение загрузки

На проверенном экземпляре выставили `fw_setenv bootdelay 5`, чтобы можно было остановить U-Boot. Сначала проверьте значение и возможность прервать загрузку через UART. Следующий файл передаёт `lxargs` в `fw_setenv` без подстановки `${partitions}` оболочкой Linux. Используйте его **только при совпадающих разделах и командах U-Boot**:

```sh
cat > /data/lx05/uboot-lxargs.txt <<'EOF'
lxargs setenv bootargs console=ttyS0,115200 root=/dev/nand0p5 rootwait init=/bin/sh partitions=${partitions} gpt=1 -- -c '"mount -text4 /dev/nand0p9 /data;exec /data/lx05/early.sh"'
EOF
fw_setenv -s /data/lx05/uboot-lxargs.txt
fw_printenv lxargs bootcmd bootdelay
sync
```

Перезагрузите устройство и остановите U-Boot. Для пробной загрузки выполните `run lxargs`, затем `printenv bootargs`: убедитесь, что `partitions` развёрнут и аргумент `-c` содержит команду монтирования `/data`. После этого выполните `run boot_second`. Существенна запись `mount -text4` без пробела: с `-t ext4` на этом U-Boot возникала ошибка `Too many args (max. 16)`. Убедитесь по UART в `LX05_EARLY_READY`, дождитесь системы и проверьте overlay, сеть и SSH. На пробном этапе `bootcmd` остаётся штатным.

Только после успешного пробного запуска на рабочем экземпляре изменили `bootcmd` из Linux:

```sh
fw_setenv bootcmd 'run lxargs boot_second'
fw_printenv bootcmd lxargs bootdelay
sync
```

После перезагрузки повторите проверки. Сохранять другие значения среды без понимания их назначения не требуется.

## Проверка изоляции и восстановление

```sh
mount | grep 'overlay on /etc'
ip rule show
ip route show table 123
ip route get 10.42.0.1          # адрес своего локального хоста
ip route get 1.1.1.1            # ожидается Host is unreachable
cat /proc/sys/net/ipv6/conf/all/disable_ipv6
cat /proc/sys/net/ipv6/conf/default/disable_ipv6
```

Ожидаются правило приоритета 100 для таблицы 123, `unreachable default`, маршруты к локальной сети, DHCP broadcast и mDNS, работающий SSH и значения IPv6 `1`. Проверьте отсутствие внешнего IPv4 и IPv6 трафика также на маршрутизаторе, в том числе после перезагрузки и перезапуска Wi-Fi. Это **policy routing, не полноценный firewall**: локальные устройства, DNS-серверы и прокси всё ещё могут пересылать трафик наружу; другие интерфейсы и сбои необходимо рассматривать отдельно. Один тест `ip route get` не доказывает защиту от OTA или утечки голоса.

Для одноразового возврата на проверенной колонке остановите U-Boot и выполните `run setargs_second boot_second`. Для постоянного возврата в U-Boot использовали `setenv bootcmd 'run setargs_second boot_second'`, `saveenv`, затем `boot`. **При штатной загрузке описанного guard нет**, поэтому сначала физически изолируйте устройство от интернета. Также ссылка на временный Wi-Fi-конфиг может остаться битой: восстановите исходный файл из своей резервной копии. Не записывайте сохранённый raw environment во flash без отдельной проверки.
