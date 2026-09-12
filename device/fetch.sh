#!/bin/sh
# Fetch one manifest or frame through the LAN-first transport.

BASE=${BASE:-/mnt/onboard/.adds/kobo-dash}
[ -r "$BASE/config" ] || BASE=$(dirname "$0")
. "$BASE/config"

PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

# dash.sh invokes this script with both streams sent to /dev/null, so anything
# worth knowing has to be written to the shared log directly. Without this a
# failed transfer reached the log only as "local failure: unspecified".
lan_log() {
    [ -n "${LOG_FILE:-}" ] || return 0
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$1" >>"$LOG_FILE" 2>/dev/null || true
}

fetch_fake() {
    kind=$1
    dest=$2
    now=$(date +%s)
    case "$kind" in
        manifest)
            next=$((now + INTERVAL_SECONDS))
            tmp="$dest.tmp.$$"
            printf '{"generated":%s,"data_sha":"%s","frame":"frame.raw.gz","next_wake_epoch":%s,"now_epoch":%s,"stale":false}\n' \
                "$now" "$FAKE_DATA_SHA" "$next" "$now" >"$tmp" || return 1
            mv "$tmp" "$dest"
            ;;
        frame)
            [ -r "$FAKE_FRAME" ] || return 1
            cp "$FAKE_FRAME" "$dest"
            ;;
        *) return 2 ;;
    esac
    return 0
}

fetch_lan() {
    kind=$1
    dest=$2
    # Two very different transfers share this path, so they get separate
    # budgets. The manifest is ~200 bytes and must fail fast, or a dead endpoint
    # eats the wake window the device is paying battery for. The frame is ~140KB
    # and is pulled seconds after the radio comes up, over a link that has not
    # settled yet: 2s was enough while the frame was ~107KB and stopped being
    # enough when the panel gained the price, value and unrealized columns.
    # A frame that times out leaves the previous image on the panel, which is
    # indistinguishable from a freeze -- the symptom that led here.
    case "$kind" in
        manifest) path=manifest.json; timeout=4 ;;
        frame) path=frame.raw.gz; timeout=20 ;;
        *) return 2 ;;
    esac
    tmp="$dest.tmp.$$"
    rm -f "$tmp"
    if ! wget -q -T "$timeout" -O "$tmp" "${LAN_ENDPOINT%/}/$path" 2>"$tmp.err"; then
        # Record why. The caller redirects this script's output to /dev/null, so
        # a swallowed error surfaced only as "local failure: unspecified".
        lan_log "lan $kind failed after ${timeout}s: $(tr '\n' ' ' <"$tmp.err" 2>/dev/null | tail -c 120)"
        rm -f "$tmp" "$tmp.err"
        return 1
    fi
    rm -f "$tmp.err"
    if [ ! -s "$tmp" ]; then
        lan_log "lan $kind returned an empty file"
        rm -f "$tmp"
        return 1
    fi
    mv "$tmp" "$dest"
}

fetch_ssh() {
    kind=$1
    dest=$2
    # Disabled unless an address was set by hand; see config.
    [ -n "$SSH_HOST" ] || return 1
    [ -x "$DBCLIENT" ] || return 1
    mkdir -p "$BASE" 2>/dev/null || true
    tmp="$dest.tmp.$$"
    rm -f "$tmp"
    # No -y: Dropbear must verify or record the host key in this USB-backed HOME.
    dbclient_cmd="HOME=\"$BASE\" \"$DBCLIENT\" -T -q -i \"$SSH_KEY\" -o ServerAliveInterval=2 -o StrictHostKeyChecking=accept-new \"$SSH_USER@$SSH_HOST\" \"$kind\""
    if command -v timeout >/dev/null 2>&1; then
        timeout 2 sh -c "$dbclient_cmd" >"$tmp" 2>/dev/null
    else
        HOME="$BASE" "$DBCLIENT" -T -q -i "$SSH_KEY" \
            -o ServerAliveInterval=2 -o StrictHostKeyChecking=accept-new \
            "$SSH_USER@$SSH_HOST" "$kind" >"$tmp" 2>/dev/null
    fi
    if [ "$?" -ne 0 ]; then
        rm -f "$tmp"
        return 1
    fi
    [ -s "$tmp" ] || { rm -f "$tmp"; return 1; }
    mv "$tmp" "$dest"
}

fetch_file() {
    kind=$1
    dest=$2
    [ "$kind" = manifest ] || [ "$kind" = frame ] || return 2
    if [ "$FAKE" = 1 ]; then
        fetch_fake "$kind" "$dest" && return 0
    fi
    if [ "$(iwgetid -r 2>/dev/null)" = "$SSID" ]; then
        fetch_lan "$kind" "$dest" && return 0
    fi
    fetch_ssh "$kind" "$dest" && return 0
    echo "remote unavailable: $kind" >&2
    return 1
}

kind=${1:-}
dest=${2:-}
if [ -z "$kind" ] || [ -z "$dest" ]; then
    echo "usage: $0 {manifest|frame} destination" >&2
    exit 2
fi
fetch_file "$kind" "$dest"
