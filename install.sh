#!/bin/bash
# Build and install into the user's home. Remove with uninstall.sh.
# Builds natively when cmake is available; otherwise inside the toolbox named
# by $TOOLBOX (default: fcitx5-build), for immutable systems.
set -euo pipefail
cd "$(dirname "$0")"
BUILD='cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build'
if command -v cmake >/dev/null; then
    bash -c "$BUILD"
else
    toolbox run -c "${TOOLBOX:-fcitx5-build}" bash -c "$BUILD"
fi
LIBDIR="$HOME/.local/lib/fcitx5"
mkdir -p "$LIBDIR" "$HOME/.local/share/fcitx5/addon"
install -m755 build/cloudsecond.so "$LIBDIR/cloudsecond.so"
sed "s|@LIBDIR@|$LIBDIR|" cloudsecond.conf.in > "$HOME/.local/share/fcitx5/addon/cloudsecond.conf"
echo "installed; restart fcitx5 to load"
