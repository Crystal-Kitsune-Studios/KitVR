#!/bin/sh
# Flatpak entry point. ffmpeg is built into /app, and the desktop app spawns the server from the
# same directory, so both live in /app/bin.
set -e
export KITVR_FFMPEG="${KITVR_FFMPEG:-/app/bin/ffmpeg}"
exec /app/bin/kitvr-desktop.real "$@"
