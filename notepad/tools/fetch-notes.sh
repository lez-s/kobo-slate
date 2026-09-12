#!/bin/sh
# Pull every notebook off the Kobo and render each one to SVG.
#
#   sh kobo/notepad/tools/fetch-notes.sh
#
# Notebooks land in iCloud Drive, so they sync to everything else on their own.
# Nothing is ever removed from the device: this only ever reads.
set -eu

HOST=${KOBO_SSH_HOST:-kobo}
REMOTE_DIR=${KOBO_NOTES_DIR:-/mnt/onboard/.adds/kobo-dash/notes}
DEST=${KOBO_NOTE_DEST:-"$HOME/Library/Mobile Documents/com~apple~CloudDocs/Documents/KoboNote"}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONVERT="$ROOT/knp2svg.py"

# The device is an appliance: between wakes it suspends with Wi-Fi off and is
# simply not there. That is the normal state, not a fault, so say so plainly
# rather than letting scp's error stand as the explanation.
if ! ssh -o ConnectTimeout=8 -o BatchMode=yes "$HOST" true 2>/dev/null; then
    echo "$HOST is not reachable." >&2
    echo "It is probably asleep between wakes -- press a page key to wake it," >&2
    echo "or wait for its next cycle, then run this again." >&2
    exit 1
fi

mkdir -p "$DEST"

names=$(ssh "$HOST" "ls -1 '$REMOTE_DIR' 2>/dev/null | grep '\.knp\$'" || true)
if [ -z "$names" ]; then
    echo "no notebooks in $HOST:$REMOTE_DIR"
    exit 0
fi

# No counters here on purpose: this loop runs in a subshell, so anything it
# incremented would be gone by the time it could be printed.
printf '%s\n' "$names" | while IFS= read -r name; do
    [ -n "$name" ] || continue
    # Archives are immutable once written -- `clear` renames the live notebook
    # aside and never touches it again -- so re-fetching one only burns time on
    # a link that is up for a few seconds. The live canvas.knp always comes
    # over: it is append-only, so each copy is a superset of the last.
    if [ "$name" != "canvas.knp" ] && [ -f "$DEST/$name" ]; then
        echo "  have  $name"
        continue
    fi
    if ! scp -q "$HOST:$REMOTE_DIR/$name" "$DEST/$name"; then
        echo "could not fetch $name" >&2
        continue
    fi
    svg="$DEST/${name%.knp}.svg"
    if python3 "$CONVERT" "$DEST/$name" -o "$svg" >/dev/null; then
        echo "  got   $name -> $(basename "$svg")"
    else
        echo "fetched $name but could not render it" >&2
    fi
done

echo
echo "notebooks in $DEST:"
ls -1 "$DEST" | sed 's/^/  /'
