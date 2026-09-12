#!/bin/sh
# One-shot, self-restoring suspend probe for MTK Kobo devices.
# The default invocation detaches so the SSH connection can disappear on suspend.

BASE=/mnt/onboard/.adds/kobo-dash
LOG=$BASE/suspend_probe.log
RTC=/sys/class/rtc/rtc0/wakealarm
AC=/sys/class/power_supply/bd71827_ac/online
EXT=/sys/power/state-extended
STATE=/sys/power/state

mkdir -p "$BASE" 2>/dev/null || exit 1

if [ "$1" != --worker ]; then
    if command -v setsid >/dev/null 2>&1; then
        setsid "$0" --worker </dev/null >>"$LOG" 2>&1 &
    else
        "$0" --worker </dev/null >>"$LOG" 2>&1 &
    fi
    printf 'suspend probe detached; follow %s\n' "$LOG"
    exit 0
fi

stamp=$(date '+%Y%m%dT%H%M%S')
PRE=$BASE/suspend_probe.$stamp.dmesg.before
POST=$BASE/suspend_probe.$stamp.dmesg.after
DELTA=$BASE/suspend_probe.$stamp.dmesg.delta
ERR=$BASE/.suspend_probe.err.$$
LOCK=$BASE/.suspend_probe.lock

if ! mkdir "$LOCK" 2>/dev/null; then
    printf '%s probe refused: another probe is running\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" >>"$LOG"
    exit 1
fi

cleanup_lock() {
    rm -rf "$LOCK" "$ERR"
}
trap cleanup_lock EXIT INT TERM

log() {
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >>"$LOG"
}

# Run one operation and preserve both its status and the shell's exact error text.
step() {
    label=$1
    shift
    : >"$ERR"
    ("$@") 2>"$ERR"
    rc=$?
    if [ -s "$ERR" ]; then
        errno_text=$(tr '\n' ' ' <"$ERR")
    else
        errno_text=none
    fi
    log "step=$label rc=$rc errno_text=$errno_text"
    return "$rc"
}

write_value() {
    value=$1
    target=$2
    printf '%s\n' "$value" >"$target"
}

read_value() {
    cat "$1"
}

capture_dmesg() {
    dmesg >"$1"
}

restore() {
    # The expected pre-probe state is state-extended=0 and no pending alarm.
    step restore_rtc_clear write_value 0 "$RTC" || true
    step restore_state_extended_0 write_value 0 "$EXT" || true
}

online=$(cat "$AC" 2>/dev/null)
if [ "$online" = 1 ]; then
    log "probe refused: external power is connected ($AC=1); unplug before running it"
    exit 2
fi
if [ "$(id -u 2>/dev/null)" != 0 ]; then
    log "probe refused: must run as root"
    exit 2
fi

log "probe begin kernel=$(uname -r) ac_online=${online:-unreadable}"
step dmesg_before capture_dmesg "$PRE" || {
    log "probe aborted: could not capture dmesg before attempt"
    exit 1
}

# Leave a short, deterministic wake path even if the suspend attempt succeeds.
now=$(date +%s)
alarm=$((now + 60))
failed=0
step rtc_clear write_value 0 "$RTC" || failed=1
step rtc_set write_value "$alarm" "$RTC" || failed=1
step rtc_readback read_value "$RTC" || failed=1
step state_extended_read_before read_value "$EXT" || failed=1
step state_extended_1 write_value 1 "$EXT" || failed=1
step state_extended_read_1 read_value "$EXT" || failed=1
step sync sync || failed=1

if [ "$failed" -eq 0 ]; then
    # Reproduce the loop's ACTUAL configuration. dash.sh powers the Wi-Fi chip
    # down immediately before suspending, so probing with Wi-Fi up would measure
    # a different scenario than the one that fails. This drops SSH -- which is
    # why the probe is detached and logs to the USB partition.
    if [ -x "$BASE/wifi.sh" ]; then
        step wifi_down "$BASE/wifi.sh" down || log "wifi down reported failure; continuing"
        sleep 1
        log "wifi_state_before_suspend=[$(iwgetid -r 2>/dev/null)] wmtWifi_off_attempted"
    else
        log "wifi.sh missing; suspending with the radio left as-is"
    fi

    # This is the one operation that may suspend the whole system.
    step state_mem write_value mem "$STATE"
    # On a refusal this is immediate; on a real suspend it runs after the RTC wake.
    step state_extended_0_resume write_value 0 "$EXT" || true
    usleep 100000

    # Bring the radio back unconditionally, so a failed probe cannot strand the
    # device with no network and no way in.
    if [ -x "$BASE/wifi.sh" ]; then
        step wifi_up "$BASE/wifi.sh" up || log "wifi up FAILED; device may need a reboot"
        log "wifi_state_after=[$(iwgetid -r 2>/dev/null)]"
    fi
else
    log "state_mem skipped because an earlier preparation step failed"
fi

# If the kernel refused entry, this runs immediately. If it entered suspend, it
# runs after the RTC wake. A hard hang/power-cycle leaves the before capture intact.
step dmesg_after capture_dmesg "$POST" || true
if [ -r "$PRE" ] && [ -r "$POST" ]; then
    diff -u "$PRE" "$POST" >"$DELTA" 2>/dev/null || true
    log "dmesg_delta=$DELTA"
else
    log "dmesg_delta unavailable: missing before or after capture"
fi

restore
step rtc_readback_after_restore read_value "$RTC" || true
step state_extended_read_after_restore read_value "$EXT" || true
log "probe end before=$PRE after=$POST delta=$DELTA"
exit 0
