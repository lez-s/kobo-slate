#!/bin/sh
# Minimal MTK Wi-Fi lifecycle outside KOReader/Nickel.

BASE=${BASE:-/mnt/onboard/.adds/kobo-dash}
[ -r "$BASE/config" ] || BASE=$(dirname "$0")
. "$BASE/config"

PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

module_loaded() {
    grep -q "^$1 " /proc/modules 2>/dev/null
}

load_modules() {
    for module in wmt_drv wmt_chrdev_wifi wmt_cdev_bt "$WIFI_MODULE"; do
        if ! module_loaded "$module"; then
            insmod "$KMOD_PATH/$module.ko" >/dev/null 2>&1 || {
                echo "wifi: failed to load $module" >&2
                return 1
            }
        fi
    done
}

# `up` must not claim success until the interface is actually usable. wpa_state
# reaches COMPLETED seconds before an address exists, and it is Nickel's own
# dhcpcd (`dhcpcd -d -z wlan0`) that ultimately assigns one, so our dhcpcd call
# returns 0 having done nothing. A measured cycle reported `up rc=0` with
# ssid=none and ip=none; dash.sh would then read a merely not-ready radio as a
# *remote* failure and back off 5-20 minutes over what needed a few more seconds.
wait_ready() {
    i=0
    while [ "$i" -lt 60 ]; do
        if [ "$(iwgetid -r 2>/dev/null)" = "$SSID" ] &&
            ifconfig "$INTERFACE" 2>/dev/null | grep -q 'inet addr:'; then
            return 0
        fi
        i=$((i + 1))
        usleep 500000
    done
    echo "wifi: associated but no address after 30s" >&2
    return 1
}

wifi_up() {
    if [ "$(iwgetid -r 2>/dev/null)" = "$SSID" ] && \
        wpa_cli -i "$INTERFACE" status 2>/dev/null | grep -q '^wpa_state=COMPLETED$'; then
        dhcpcd -d -t 30 -w "$INTERFACE" >/dev/null 2>&1 || true
        wait_ready
        return $?
    fi
    load_modules || return 1

    # These debug toggles are the verified mt8113t-ntx power-on sequence.
    echo "0xDB9DB9" >/proc/driver/wmt_dbg || return 1
    echo "7 9 0" >/proc/driver/wmt_dbg || return 1
    sleep 1
    echo "0xDB9DB9" >/proc/driver/wmt_dbg || return 1
    echo "7 9 1" >/proc/driver/wmt_dbg || return 1
    echo 1 >/dev/wmtWifi || return 1
    usleep 250000
    sleep 1
    ifconfig "$INTERFACE" up || return 1

    if ! pkill -0 wpa_supplicant 2>/dev/null; then
        wpa_supplicant -D "$WPA_SUPPLICANT_DRIVER" -s -i "$INTERFACE" \
            -c /etc/wpa_supplicant/wpa_supplicant.conf \
            -C /var/run/wpa_supplicant -B >/dev/null 2>&1 || return 1
    fi

    i=0
    while [ "$i" -lt 60 ]; do
        if wpa_cli -i "$INTERFACE" status 2>/dev/null | grep -q '^wpa_state=COMPLETED$'; then
            break
        fi
        i=$((i + 1))
        usleep 250000
    done
    [ "$i" -lt 60 ] || {
        echo "wifi: association timeout" >&2
        return 1
    }

    # Match KOReader's verified DHCP path. dhcpcd is deliberately not killed
    # by this project except when wifi down is explicitly requested.
    dhcpcd -d -t 30 -w "$INTERFACE" >/dev/null 2>&1 || true
    wait_ready
}

wifi_down() {
    dhcpcd -d -k "$INTERFACE" >/dev/null 2>&1 || true
    wpa_cli -i "$INTERFACE" terminate >/dev/null 2>&1 || true
    ifconfig "$INTERFACE" down >/dev/null 2>&1 || true
    echo 0 >/dev/wmtWifi || return 1
    return 0
}

wifi_status() {
    ssid=$(iwgetid -r 2>/dev/null)
    state=$(wpa_cli -i "$INTERFACE" status 2>/dev/null | sed -n 's/^wpa_state=//p' | head -n 1)
    ip=$(ifconfig "$INTERFACE" 2>/dev/null | sed -n 's/.*inet addr:\([^ ]*\).*/\1/p' | head -n 1)
    [ -n "$ssid" ] || ssid=none
    [ -n "$state" ] || state=unknown
    [ -n "$ip" ] || ip=none
    if module_loaded "$WIFI_MODULE"; then modules=loaded; else modules=down; fi
    printf 'wifi status: ssid=%s state=%s ip=%s modules=%s\n' "$ssid" "$state" "$ip" "$modules"
    [ "$state" = COMPLETED ]
}

case "${1:-status}" in
    up) wifi_up ;;
    down) wifi_down ;;
    status) wifi_status ;;
    *) echo "usage: $0 {up|down|status}" >&2; exit 2 ;;
esac
