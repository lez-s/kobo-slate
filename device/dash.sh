#!/bin/sh
# Kobo dashboard loop. Phase 0 uses FAKE=1 and DEV=1 by default.

BASE=${BASE:-/mnt/onboard/.adds/kobo-dash}
[ -r "$BASE/config" ] || BASE=$(dirname "$0")
. "$BASE/config"

PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

# Defaulted here and not only in config.example: `config` on a device is local
# state that deployment never overwrites, so a device installed before this
# option existed would otherwise source a config that leaves it empty.
BATTERY_CAPACITY=${BATTERY_CAPACITY:-/sys/class/power_supply/bd71827_bat/capacity}
# Defaulted here for the same reason: `config` on a device is local state that
# deployment never overwrites, so an older install still finds the notepad.
NOTEPAD=${NOTEPAD:-/mnt/onboard/.adds/kobo-dash/notepad}

mkdir -p "$STATE_DIR" 2>/dev/null || exit 1

# NickelMenu's cmd_spawn fires on every tap and nothing debounces it. Two loops
# fight over the framebuffer, the state dir and the radio -- one brings Wi-Fi up
# while the other takes it down. Observed on the first real launch: four dash.sh
# processes and two "cycle=1" lines eight seconds apart. mkdir is the atomic
# primitive busybox always has; flock is not guaranteed here.
# The page keys and the power key are separate wakeup sources, each with its
# own event counter, and sampling them either side of a suspend is the only way
# this loop can tell which one woke it: the input event that caused the wake is
# not buffered for a process that had nothing open on the device at the time.
#
# Paths are discovered rather than written out because the power key's sits
# behind an i2c address that firmware is free to renumber. Discovery happens
# once here, not per cycle.
resolve_wakeup_counter() {
    for candidate in $(find /sys/devices -path "*$1*/power/wakeup_count" 2>/dev/null); do
        [ -r "$candidate" ] || continue
        printf '%s\n' "$candidate"
        return 0
    done
    return 1
}
PAGE_WAKEUP_COUNT=${PAGE_WAKEUP_COUNT:-$(resolve_wakeup_counter gpio-keys)}
POWER_WAKEUP_COUNT=${POWER_WAKEUP_COUNT:-$(resolve_wakeup_counter pwrkey)}

# The counters name the device that woke the loop, and both page keys share
# one, so telling up from down needs the key code. A reader opened before the
# suspend keeps its evdev buffer across it -- the process is frozen, not
# closed -- so the press that caused the wake is still queued on resume. This
# names which key it was, and that is what selects the page: KEY_F23 (193)
# refreshes the dashboard, KEY_F24 (194) hands the panel to the notepad.
#
# The capture lives on tmpfs. It is a few bytes per wake, but this device boots
# from flash with a finite write budget, which is why the frame is decompressed
# twice rather than staged to a file.
PAGE_EVENT=${PAGE_EVENT:-/dev/input/event0}
KEY_CAPTURE=${KEY_CAPTURE:-/tmp/kobo-dash-keys.bin}

LOCK="$STATE_DIR/.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    holder=$(cat "$LOCK/pid" 2>/dev/null)
    # The winner writes its pid a moment *after* mkdir succeeds, so an empty pid
    # means "just acquired", not "abandoned". Re-read once before judging:
    # without this, two instances racing the same window both conclude the lock
    # is stale, both rm -rf it, and both run -- which is what the lock exists to
    # prevent. Observed as a vanished .lock dir while a loop was still running.
    if [ -z "$holder" ]; then
        usleep 500000
        holder=$(cat "$LOCK/pid" 2>/dev/null)
    fi
    if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then
        printf '%s already running as pid %s; exiting\n' \
            "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$holder" >>"$LOG_FILE"
        exit 0
    fi
    # Stale lock: the holder died with the device, e.g. a long-press reboot.
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || exit 1
fi
echo $$ >"$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT INT TERM

# One line of launch context per run. This device is reachable for only a few
# seconds per cycle, so a misbehaving launch can rarely be inspected live. A
# NickelMenu spawn and an SSH spawn differ in parent, working directory and
# inherited environment -- LD_LIBRARY_PATH in particular, since Nickel sets it
# to its own Qt tree and a child that inherits it can fail in ways that look
# like a logic bug. Recording it makes the two launch paths comparable after
# the fact instead of by argument.
printf '%s launch pid=%s cwd=%s dev=%s fake=%s ld=%s\n' \
    "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$$" "$(pwd)" "${DEV:-?}" "${FAKE:-?}" \
    "${LD_LIBRARY_PATH:-unset}" >>"$LOG_FILE"

log_line() {
    # Keep operational logs compact: one line per completed cycle.
    printf '%s cycle=%s result=%s remote=%s local=%s sha=%s elapsed=%ss next=%s\n' \
        "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$1" "$2" "$3" "$4" "$5" "$6" "$7" >>"$LOG_FILE"
}

state_read() {
    [ -r "$STATE_DIR/$1" ] && cat "$STATE_DIR/$1"
}

state_write() {
    tmp="$STATE_DIR/$1.tmp.$$"
    printf '%s\n' "$2" >"$tmp" && mv "$tmp" "$STATE_DIR/$1"
}

# -1 means "unreadable", which classify_wake treats as "cannot tell" rather
# than folding into an arithmetic delta that would look like a real answer.
read_wakeup_counter() {
    value=$(cat "$1" 2>/dev/null)
    case "$value" in ''|*[!0-9]*) value=-1 ;; esac
    printf '%s\n' "$value"
}

# Which key woke the device, written for the next iteration to read.
#
# Deliberately biased: anything other than an unambiguous page-key press is
# recorded as "power". A page press misread as power costs one missed refresh;
# a power press misread as page costs the only way out of the loop short of a
# 30-second forced reset, which is the failure this ordering exists to avoid.
classify_wake() {
    # Defaulted so a missing counter path degrades to "cannot tell" instead of
    # making the comparisons below error out on an empty string.
    page_before=${page_before:--1}
    power_before=${power_before:--1}
    page_after=$(read_wakeup_counter "$PAGE_WAKEUP_COUNT")
    power_after=$(read_wakeup_counter "$POWER_WAKEUP_COUNT")
    source=power
    if [ "$page_before" -ge 0 ] && [ "$power_before" -ge 0 ] &&
       [ "$page_after" -ge 0 ] && [ "$power_after" -ge 0 ]; then
        if [ "$page_after" -gt "$page_before" ] && [ "$power_after" -le "$power_before" ]; then
            source=page
        fi
    else
        source=unknown
    fi
    state_write wake_source "$source"
    note "wake source=$source page=${page_before}->${page_after} power=${power_before}->${power_after}"
}

start_key_capture() {
    key_reader_pid=""
    [ -r "$PAGE_EVENT" ] || return 0
    rm -f "$KEY_CAPTURE"
    cat "$PAGE_EVENT" >"$KEY_CAPTURE" 2>/dev/null &
    key_reader_pid=$!
}

# Records are 16 bytes on this 32-bit userspace: a four-word timeval, then
# type, code and value. Only EV_KEY (type 1) records are logged. A raw dump was
# tried first and truncated the interesting record: this device emits two
# EV_REP configuration records ahead of the press, so a fixed window at the
# start of the file caught the settings and missed the key.
#
# The two page keys report 193 and 194 (KEY_F23 and KEY_F24); the power key is
# a different device and never appears here.
stop_key_capture() {
    if [ -z "$key_reader_pid" ]; then
        state_write wake_key ""
        return 0
    fi
    kill "$key_reader_pid" 2>/dev/null
    wait "$key_reader_pid" 2>/dev/null
    key_reader_pid=""
    bytes=$(wc -c <"$KEY_CAPTURE" 2>/dev/null)
    case "$bytes" in ''|*[!0-9]*) bytes=0 ;; esac
    if [ "$bytes" -eq 0 ]; then
        state_write wake_key ""
        return 0
    fi
    keys=$(od -An -tu2 -v "$KEY_CAPTURE" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' |
        awk 'NR%8==5 {type=$1} NR%8==6 {code=$1} NR%8==7 && type==1 {printf "%s:%s ", code, $1}')
    wake_key=$(printf '%s\n' "$keys" | awk '{for (i=1; i<=NF; i++) { split($i, pair, ":"); if ((pair[1] == 193 || pair[1] == 194) && pair[2] == 1) key=pair[1] }} END {print key}')
    state_write wake_key "${wake_key:-}"
    note "key capture bytes=$bytes keys=[${keys:-none}]"
}

# The notepad owns the panel while it runs. Its status 10 means deliberate exit
# to Nickel; all other statuses return to the dashboard refresh path.
run_notepad() {
    [ -x "$NOTEPAD" ] || return 1
    "$NOTEPAD"
    status=$?
    note "notepad exited code=$status"
    return "$status"
}

# Both loops use status 10 for the user's deliberate "exit to Nickel" intent;
# this loop owns the lock, cleanup trap and reboot rather than the notepad.
exit_to_nickel() {
    "$FBINK" -q -m -M -h "  Exiting to Nickel...  " >/dev/null 2>&1 || true
    state_write last_early_wake 0
    sync
    reboot
    exit 0
}

# Put the dashboard back on the panel from the cached frame, without a fetch.
#
# The notepad painted over the dashboard, and the sha gate below only repaints
# once a *fetch* has succeeded. A fetch is exactly what fails when nobody's
# phone is on the LAN, and the remote backoff is 5, 10, then 20 minutes -- so
# waiting for one would make "I pressed exit and the panel kept showing my
# notes" the common case rather than the rare one. Painting the cached frame
# here also leaves the ordinary sha gate intact: an unchanged sha still reports
# `unchanged`, instead of flashing the very same image a second time.
#
# Variable names are deliberately distinct from the loop's own `frame` and from
# `stop_key_capture`'s `bytes`: everything here is a global in /bin/sh.
paint_cached_frame() {
    cached_frame="$STATE_DIR/frame.raw.gz"
    [ -r "$cached_frame" ] || return 1
    # `| tr -d " "` because a wc that pads its output (BSD, and so any
    # host-side test of this function) would otherwise fail the numeric
    # guard below and make a good frame look unusable. Busybox does not
    # pad, so this only ever matters off-device.
    cached_bytes=$(gzip -dc "$cached_frame" 2>/dev/null | wc -c | tr -d ' ')
    case "$cached_bytes" in ''|*[!0-9]*) return 1 ;; esac
    [ "$cached_bytes" -eq "$FRAME_BYTES" ] || return 1
    gzip -dc "$cached_frame" >"$FB_DEVICE" 2>/dev/null || return 1
    "$FBINK" -s -f >/dev/null 2>&1 || return 1
}

frontlight_off() {
    ok=0
    # Real node names on this device are lm3630a_led, lm3630a_leda, lm3630a_ledb
    # -- no underscore before a/b. The underscored spellings never existed, so
    # two of the three iterations had been silently doing nothing.
    for light in /sys/class/backlight/lm3630a_led /sys/class/backlight/lm3630a_leda /sys/class/backlight/lm3630a_ledb; do
        if [ -e "$light/brightness" ]; then
            echo 0 >"$light/brightness" || ok=1
        fi
    done
    return "$ok"
}

# Nickel repaints the framebuffer and runs its own power management, so it has
# to be gone before the loop can own the panel. This is koreader.sh's kill list
# MINUS dhcpcd and dhcpcd-dbus. KOReader kills those deliberately because it
# brings up its own per-interface DHCP; we do not. Nickel's `dhcpcd -d -z wlan0`
# is what actually assigns our address (verified: wifi.sh up only obtains an IP
# while that daemon is alive), and keeping it also preserves SSH during dev.
stop_nickel() {
    sync
    killall -q -TERM nickel hindenburg sickel fickel strickel fontickel \
        adobehost foxitpdf iink bluealsa bluetoothd fmon nanoclock.lua
    i=0
    while pkill -0 nickel 2>/dev/null; do
        [ "$i" -ge 15 ] && break
        i=$((i + 1))
        usleep 250000
    done
    pkill -0 nickel 2>/dev/null && return 1
    return 0
}

local_failure() {
    reason=${1:-${step_error:-unspecified}}
    count=$(state_read local_failures)
    case "$count" in ''|*[!0-9]*) count=0 ;; esac
    count=$((count + 1))
    state_write local_failures "$count"
    # Log every failure with its cause, not just the threshold. Logging only the
    # threshold produced a reboot with no recorded reason, which made a real
    # suspend fault impossible to diagnose after the fact.
    printf '%s local failure %s/%s: %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" \
        "$count" "$LOCAL_FAILURE_THRESHOLD" "$reason" >>"$LOG_FILE"
    step_error=""
    if [ "$DEV" != 1 ] && [ "$count" -ge "$LOCAL_FAILURE_THRESHOLD" ]; then
        printf '%s local failure threshold reached; rebooting to Nickel\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" >>"$LOG_FILE"
        reboot
        exit 1
    fi
}

remote_failure_delay() {
    index=$1
    delay=300
    i=0
    for candidate in $BACKOFF_SECONDS; do
        [ "$i" -eq "$index" ] && delay=$candidate
        i=$((i + 1))
    done
    printf '%s\n' "$delay"
}

manifest_field() {
    field=$1
    file=$2
    case "$field" in
        data_sha|frame)
            sed -n "s/.*\"$field\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | head -n 1
            ;;
        *)
            sed -n "s/.*\"$field\"[[:space:]]*:[[:space:]]*\([^,}]*\).*/\1/p" "$file" | head -n 1
            ;;
    esac
}

valid_manifest() {
    file=$1
    sha=$(manifest_field data_sha "$file")
    next=$(manifest_field next_wake_epoch "$file")
    now=$(manifest_field now_epoch "$file")
    case "$sha" in ''|*[!A-Za-z0-9_.:-]*) return 1 ;; esac
    case "$next:$now" in *[!0-9:]*|*:*:*) return 1 ;; esac
    [ -n "$next" ] && [ -n "$now" ]
}

# Wait in seconds, taken from the manifest's own clock domain. Using the
# server's delta rather than its absolute epoch stays correct while the device
# clock drifts, which it does: there is no NTP once Nickel is stopped.
manifest_delay() {
    manifest=$1
    next=$(manifest_field next_wake_epoch "$manifest")
    manifest_now=$(manifest_field now_epoch "$manifest")
    delta=$((next - manifest_now))
    [ "$delta" -lt 1 ] && delta=1
    printf '%s\n' "$delta"
}

# The radio is powered down only because suspend requires it. When we are not
# going to suspend -- DEV mode, or plugged in, where this MTK kernel hangs on
# suspend anyway -- shutting Wi-Fi off buys nothing and costs SSH access for all
# but a few seconds per cycle, which makes the device miserable to work on.
# Plugged-in detection MUST come from the AC supply, not the battery status.
# A fully charged but still-connected device reports status=Full, not Charging,
# so a `= Charging` test lets the loop suspend while plugged in -- exactly the
# MTK kernel hang this guard exists to prevent. KOReader makes the same point
# in device.lua: "isCharging is still true while isCharged".
on_external_power() {
    [ "$(cat "$AC_ONLINE" 2>/dev/null)" = 1 ] && return 0
    # Fall back to the battery node if the AC node is missing on some firmware.
    case "$(cat "$BATTERY_STATUS" 2>/dev/null)" in
        Charging|Full|"Not charging") return 0 ;;
    esac
    return 1
}

# Battery can only be stamped from here. The panel is rendered on the server,
# which has no way to know this device's charge, and the mailbox transport is
# read-only, so there is no path for the device to report it upstream.
battery_stamp() {
    pct=$(cat "$BATTERY_CAPACITY" 2>/dev/null)
    case "$pct" in ''|*[!0-9]*) printf 'bat --'; return ;; esac
    if on_external_power; then
        printf 'bat %s%% +' "$pct"
    else
        printf 'bat %s%%' "$pct"
    fi
}

will_suspend() {
    [ "$DEV" = 1 ] && return 1
    on_external_power && return 1
    return 0
}

note() {
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >>"$LOG_FILE"
}

# `echo mem` succeeds from a standalone probe but is refused here. The probe
# happened to sleep 1s between powering the radio down and writing mem; this
# loop wrote immediately. KOReader's standby path also settles (usleep 90000)
# with a comment that the hardware "needs some time to settle".
#
# Rather than assume the delay is the cause, retry with an escalating settle and
# log every attempt: the next run's log then states whether a settle fixes it
# (first refused, second accepted) or rules the theory out (all three refused).
suspend_to_mem() {
    # Cleared per call: /bin/sh has no locals, so without this the list would
    # accumulate every refused settle since the loop started.
    refused=""
    # Measured on device: a 1s settle was refused on every single cycle and 3s
    # was accepted on every single one, so starting at 1 only bought a guaranteed
    # failed write plus a wasted second per wake (~5 min/day at a 5-minute
    # interval). Start where it actually works and keep headroom above it.
    for settle in 3 6 12; do
        sleep "$settle"
        # Measure, don't infer. "Entered suspend" says nothing about how long the
        # device actually stayed down; the wakeup_count delta says how many
        # wakeup events fired while it was there, which is what identifies a
        # subsystem that resumes the device immediately.
        wc_before=$(cat /sys/power/wakeup_count 2>/dev/null)
        t0=$(date +%s)
        if echo mem >/sys/power/state 2>/dev/null; then
            t1=$(date +%s)
            wc_after=$(cat /sys/power/wakeup_count 2>/dev/null)
            note "suspend ok settle=${settle}s slept=$((t1 - t0))s requested=${1:-?}s wakeup_count=${wc_before}->${wc_after}"
            return 0
        fi
        note "mem refused after ${settle}s settle"
        refused="${refused:+$refused, }${settle}s"
    done
    # Built from the loop rather than restated: the settles were retuned to
    # 3 6 12 and this message still claimed "1s, 3s and 8s" long afterwards.
    step_error="write mem > /sys/power/state (refused after $refused settles)"
    return 1
}

arm_rtc() {
    delay=$1
    echo 0 >"$RTC" 2>/dev/null || { step_error="rtc clear failed: $RTC"; return 1; }
    echo $(( $(date +%s) + delay )) >"$RTC" 2>/dev/null ||
        { step_error="rtc set failed: $RTC"; return 1; }
}

# Every suspend MUST arm the RTC first. Suspending without an alarm sleeps until
# a physical key press, which stops the dashboard with no outward sign.
sleep_or_suspend() {
    delay=$1
    [ "$delay" -lt 1 ] && delay=1
    if [ "$DEV" = 1 ]; then
        state_write wake_source timer
        sleep "$delay"
        return 0
    fi
    if on_external_power; then
        # Suspending while plugged in hangs this MTK kernel. Nothing can wake a
        # plain sleep either, so there is no manual refresh while on charge.
        state_write wake_source timer
        sleep "$delay"
        return 0
    fi
    arm_rtc "$delay" || return 1
    start_key_capture
    page_before=$(read_wakeup_counter "$PAGE_WAKEUP_COUNT")
    power_before=$(read_wakeup_counter "$POWER_WAKEUP_COUNT")
    # Each step names itself, so a failed suspend says which write was refused
    # instead of just incrementing an opaque counter.
    echo 1 >/sys/power/state-extended 2>/dev/null ||
        { step_error="write 1 > /sys/power/state-extended"; return 1; }
    sync || { step_error="sync"; return 1; }
    suspend_to_mem
    suspend_status=$?
    # Both run before any early return. The classification is what the next
    # cycle needs, and the reader holds the event device open -- leaving it
    # alive on a refused suspend would stack one more of them every cycle.
    classify_wake
    stop_key_capture
    [ "$suspend_status" -eq 0 ] || return 1
    echo 0 >/sys/power/state-extended 2>/dev/null ||
        { step_error="write 0 > /sys/power/state-extended (on resume)"; return 1; }
    usleep 100000
}

# DEV mode deliberately leaves Nickel running so the loop stays exercisable
# over SSH. Refusing to continue is better than fighting Nickel for the panel.
if [ "$DEV" != 1 ]; then
    stop_nickel || {
        printf '%s nickel did not stop; refusing to fight it for the panel\n' \
            "$(date '+%Y-%m-%dT%H:%M:%S%z')" >>"$LOG_FILE"
        exit 1
    }
fi

cycle=0
remote_backoff=0
# Forces a paint on the first cycle; see the gate below.
first_cycle=1
while :; do
    cycle=$((cycle + 1))
    started=$(date +%s)
    remote_result=ok
    local_result=ok
    result=ok
    # Empty on a scheduled wake; set when a key started this cycle, so the
    # stamp line says which key the panel is answering.
    wake_note=""
    manifest="$STATE_DIR/manifest.json"
    downloaded_manifest="$STATE_DIR/manifest.new.json"
    frame="$STATE_DIR/frame.raw.gz"
    last_sha=$(state_read data_sha)
    next_saved=$(state_read wake_target)

    # A wake before the scheduled alarm was caused by a person: the RTC is the
    # only other enabled source. Which key it was decides what happens, and
    # classify_wake wrote that verdict on the way out of the last suspend.
    #
    # Both the page keys and the power key can wake this device -- an earlier
    # note here claimed gpio-keys reported wakeup=n/a and that every early wake
    # was therefore the power key, which was wrong: gpio-keys reports
    # wakeup=enabled, and two page presses used to reboot to Nickel by accident.
    #
    # Silently going back to sleep makes a key look dead: press it, the device
    # wakes for a second and suspends again before anything appears. That is
    # what forced a 30-second hard reset during testing, since the loop has no
    # other exit.
    # first_cycle is part of the condition because wake_target survives in the
    # state dir across a reboot. A launch from NickelMenu minutes after the last
    # run therefore looks exactly like an early wake, and the branch would draw
    # the exit hint and suspend again without ever painting -- leaving Nickel's
    # last frame on screen, which is what a freeze looks like.
    # Must track config.example's default. The two disagreed from the commit
    # that introduced them -- 30 there and in the README, 15 here -- and since
    # `config` on a device never sets this, the fallback is what actually ran:
    # the documented 30 had never once taken effect.
    window=${EXIT_PRESS_WINDOW:-30}
    if [ "$DEV" != 1 ] && [ "$first_cycle" != 1 ] && [ -n "$next_saved" ]; then
        now=$(date +%s)
        if [ "$now" -lt $((next_saved - 30)) ]; then
            wake_source=$(state_read wake_source)
            if [ "$wake_source" = page ]; then
                # F24 is the second page: let the notepad own the panel until it
                # exits. F23 and unknown values keep the established refresh path.
                wake_key=$(state_read wake_key)
                if [ "$wake_key" = 194 ]; then
                    note "page key pressed; opening notepad"
                    wake_note="  via notepad"
                    state_write last_early_wake 0
                    run_notepad
                    notepad_status=$?
                    if [ "$notepad_status" -eq 10 ]; then
                        exit_to_nickel
                    fi
                    # Restore the dashboard immediately; see paint_cached_frame.
                    # Only when there is no usable cached frame does this fall
                    # back to forcing the gate open. Note it resets the in-memory
                    # flag and not data_sha: last_sha was read at the top of this
                    # cycle, so clearing the file here would change nothing.
                    paint_cached_frame || first_cycle=1
                    if [ "$notepad_status" -ne 0 ]; then
                        note "notepad failed or unavailable; refreshing on demand"
                        wake_note="  via page key"
                        # A failed or absent notepad did not promise a panel paint;
                        # use the ordinary acknowledgement before the refresh.
                        "$FBINK" -q -y -2 -m -p "refreshing..." >/dev/null 2>&1 || true
                    fi
                else
                    # F23 means "show me the current frame now". The press is
                    # deliberately not counted toward the exit, so keys knocked in a
                    # bag refresh the panel instead of rebooting it.
                    note "page key pressed; refreshing on demand"
                    wake_note="  via page key"
                    state_write last_early_wake 0
                    # Acknowledged before the fetch, not after: bringing the radio
                    # up and pulling a frame takes several seconds, and an unmarked
                    # pause is indistinguishable from a key that did nothing.
                    "$FBINK" -q -y -2 -m -p "refreshing..." >/dev/null 2>&1 || true
                    # Falls through into the ordinary cycle below, which fetches,
                    # paints, and takes a fresh schedule from the new manifest.
                fi
            else
                last_press=$(state_read last_early_wake)
                case "$last_press" in ''|*[!0-9]*) last_press=0 ;; esac
                if [ "$last_press" -gt 0 ] && [ $((now - last_press)) -le "$window" ]; then
                    note "power key pressed twice within ${window}s; exiting to Nickel"
                    exit_to_nickel
                fi
                state_write last_early_wake "$now"
                # Wording matters here. A press only counts as a wake event while
                # the device is suspended; pressing again during the 2-3s the loop
                # is awake drawing this hint does nothing, because nothing reads
                # evdev. Telling the user to wait is what makes the exit reliable.
                # The power key stays a pure hint rather than also refreshing, so
                # the second press is never queued behind a fetch.
                "$FBINK" -q -y -4 -m -p \
                    "EXIT? wait ~3s, then press power again (${window}s)" >/dev/null 2>&1 || true
                sleep_or_suspend $((next_saved - now)) || local_failure
                continue
            fi
        fi
    fi

    frontlight_off || { local_result=failed; local_failure; }

    if [ "$DEV" != 1 ] || [ "$(iwgetid -r 2>/dev/null)" != "$SSID" ]; then
        "$BASE/wifi.sh" up >/dev/null 2>&1 || { local_result=failed; local_failure; }
    fi

    if [ "$local_result" = ok ]; then
        rm -f "$downloaded_manifest"
        if "$BASE/fetch.sh" manifest "$downloaded_manifest" >/dev/null 2>&1 && valid_manifest "$downloaded_manifest"; then
            mv "$downloaded_manifest" "$manifest"
        else
            remote_result=failed
            rm -f "$downloaded_manifest"
        fi
    fi

    # A remote outage leaves the cached manifest in place, so the old schedule
    # and frame survive rather than becoming all-night five-minute wakeups.
    if [ ! -r "$manifest" ] || ! valid_manifest "$manifest"; then
        result=remote-unavailable
        remote_result=failed
        delay=$(remote_failure_delay "$remote_backoff")
        [ "$remote_backoff" -lt 2 ] && remote_backoff=$((remote_backoff + 1))
        will_suspend && "$BASE/wifi.sh" down >/dev/null 2>&1
        elapsed=$(( $(date +%s) - started ))
        log_line "$cycle" "$result" "$remote_result" "$local_result" "${last_sha:-none}" "$elapsed" none
        state_write wake_target $(( $(date +%s) + delay ))
        sleep_or_suspend "$delay" || local_failure
        continue
    fi

    sha=$(manifest_field data_sha "$manifest")
    next=$(manifest_field next_wake_epoch "$manifest")
    # The first cycle after launch must always paint. Nickel has just been
    # drawing on this panel, so a cached sha says nothing about what is actually
    # on screen. Trusting it leaves the user staring at Nickel's last frame,
    # which is indistinguishable from a freeze -- observed on the first real
    # menu launch, where leftover state made cycle 1 report "unchanged".
    if [ "$sha" != "$last_sha" ] || [ "$first_cycle" = 1 ]; then
        if [ "$FAKE" = 1 ] && [ -r "$FAKE_FRAME" ]; then
            cp "$FAKE_FRAME" "$frame"
        else
            "$BASE/fetch.sh" frame "$frame" >/dev/null 2>&1 || frame=""
        fi
        if [ -z "$frame" ] || [ ! -r "$frame" ]; then
            remote_result=failed
            result=remote-unavailable
        else
            # Deliberately decompressed twice: validating via a temp file would
            # write ~8 MB to flash every refresh. Spending CPU instead of flash
            # wear is the right trade here, and it keeps a truncated frame from
            # ever reaching the panel.
            bytes=$(gzip -dc "$frame" 2>/dev/null | wc -c)
            if [ "$bytes" -ne "$FRAME_BYTES" ]; then
                local_result=failed
                result=local-failure
                local_failure "frame decompressed to $bytes bytes, expected $FRAME_BYTES"
            elif gzip -dc "$frame" >"$FB_DEVICE" 2>/dev/null && "$FBINK" -s -f >/dev/null 2>&1; then
                state_write data_sha "$sha"
                last_sha=$sha
                # Cleared only after a successful paint, so a failed first
                # paint keeps retrying instead of silently going quiet.
                first_cycle=0
            else
                local_result=failed
                result=local-failure
                # The two commands fail for unrelated reasons -- a refused write
                # to /dev/fb0 versus an fbink refresh that cannot drive the
                # panel -- so name both rather than logging "unspecified" and
                # leaving the next reader to guess which half broke.
                local_failure "write to $FB_DEVICE or $FBINK refresh failed"
            fi
        fi
    else
        result=unchanged
    fi

    # Stamp the panel every cycle, independent of the sha gate. The full frame is
    # only redrawn when the data changes, so without this the screen shows a
    # fixed time and there is no way to tell from the device alone whether a wake
    # actually happened. A small partial update also avoids a full-screen flash.
    "$FBINK" -q -y -2 -m -p \
        "updated $(date '+%Y-%m-%d %H:%M:%S')  cycle $cycle  $(battery_stamp)$wake_note" \
        >/dev/null 2>&1 || true

    if [ "$remote_result" = failed ] && [ "$result" = ok -o "$result" = unchanged ]; then
        result=remote-stale
    fi
    if [ "$remote_result" = ok ]; then
        remote_backoff=0
    fi
    if will_suspend; then
        "$BASE/wifi.sh" down >/dev/null 2>&1 || { local_result=failed; local_failure; }
    fi

    delay=$(manifest_delay "$manifest")
    if [ "$remote_result" = failed ]; then
        delay=$(remote_failure_delay "$remote_backoff")
        [ "$remote_backoff" -lt 2 ] && remote_backoff=$((remote_backoff + 1))
    fi
    state_write wake_target $(( $(date +%s) + delay ))
    if [ "$result" = ok ] || [ "$result" = unchanged ]; then
        state_write local_failures 0
    fi
    elapsed=$(( $(date +%s) - started ))
    log_line "$cycle" "$result" "$remote_result" "$local_result" "${sha:-${last_sha:-none}}" "$elapsed" "$next"
    sleep_or_suspend "$delay" || { local_failure; sleep 60; }
done
