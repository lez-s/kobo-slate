#!/bin/sh
set -eu

# Cross-compiles the device binary on a remote ARM toolchain host, because the
# Kobo needs arm-linux-gnueabihf and a Mac has no such cross toolchain. Set
# BUILD_HOST to an ssh target that has both that compiler and a built FBInk:
#
#   BUILD_HOST=my-build-box sh build.sh
#
# The remote paths are relative to that account's home directory, so nothing
# here assumes a particular username.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -z "${BUILD_HOST:-}" ]; then
    echo "build.sh: set BUILD_HOST to an ssh target with arm-linux-gnueabihf-gcc and FBInk" >&2
    exit 2
fi
REMOTE=${NOTEPAD_BUILD_DIR:-notepad-build}
REMOTE_SRC="$REMOTE/src"
REMOTE_INC=${FBINK_DIR:-fbink-build/FBInk}
REMOTE_LIB=${FBINK_LIB_DIR:-"$REMOTE_INC/Release"}

mkdir -p "$ROOT/build"
ssh "$BUILD_HOST" "mkdir -p '$REMOTE_SRC'"
if ! rsync -az -e 'ssh' "$ROOT/notepad.c" "$ROOT/notepad.h" "$ROOT/store.c" "$ROOT/canvas.c" "$ROOT/gesture.c" "$ROOT/gesture.h" "$BUILD_HOST:$REMOTE_SRC/"; then
    # The minimal build account may not include rsync on its remote PATH.
    for source in notepad.c notepad.h store.c canvas.c gesture.c gesture.h; do
        ssh "$BUILD_HOST" "cat > '$REMOTE_SRC/$source'" < "$ROOT/$source"
    done
fi
ssh "$BUILD_HOST" "arm-linux-gnueabihf-gcc -std=c99 -Wall -Wextra -Werror -static -O2 '$REMOTE_SRC/notepad.c' '$REMOTE_SRC/store.c' '$REMOTE_SRC/canvas.c' '$REMOTE_SRC/gesture.c' -I'$REMOTE_INC' -L'$REMOTE_LIB' -l:libfbink.a -lm -o '$REMOTE/notepad'"
scp "$BUILD_HOST:$REMOTE/notepad" "$ROOT/build/notepad"
chmod +x "$ROOT/build/notepad"
file "$ROOT/build/notepad"
