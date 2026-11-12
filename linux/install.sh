#!/bin/sh
# Installs (or removes) the KitVR Vulkan layer and the vrcompositor wrapper in a SteamVR install.
#
#   ./install.sh                 install into the default SteamVR location
#   ./install.sh --uninstall     restore the original vrcompositor
#   STEAMVR_DIR=/path ./install.sh
#
# Build the layer first:
#   cmake -S linux/vklayer -B linux/vklayer/build
#   cmake --build linux/vklayer/build
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="${STEAMVR_DIR:-$HOME/.steam/steam/steamapps/common/SteamVR}/bin/linux64"
LAYER_DEST="$BIN_DIR/kitvr-layer"

# Works from a source checkout (vklayer/build) and from the release tarball, where the library and
# manifest sit next to this script.
LAYER_SRC=""
for candidate in "$HERE/vklayer/build" "$HERE" "$HERE/kitvr-layer"; do
    if [ -f "$candidate/libkitvr_layer.so" ] && [ -f "$candidate/kitvr_layer.json" ]; then
        LAYER_SRC="$candidate"
        break
    fi
done

if [ ! -d "$BIN_DIR" ]; then
    echo "SteamVR bin directory not found at $BIN_DIR" >&2
    echo "Set STEAMVR_DIR to the SteamVR install directory." >&2
    exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
    if [ -f "$BIN_DIR/vrcompositor.real" ]; then
        rm -f "$BIN_DIR/vrcompositor"
        mv "$BIN_DIR/vrcompositor.real" "$BIN_DIR/vrcompositor"
        echo "Restored $BIN_DIR/vrcompositor"
    else
        echo "No vrcompositor.real found, nothing to restore."
    fi
    rm -rf "$LAYER_DEST"
    echo "Removed the KitVR layer."
    exit 0
fi

if [ -z "$LAYER_SRC" ]; then
    echo "Could not find libkitvr_layer.so and kitvr_layer.json next to this script." >&2
    echo "Build the layer first:" >&2
    echo "  cmake -S linux/vklayer -B linux/vklayer/build" >&2
    echo "  cmake --build linux/vklayer/build" >&2
    exit 1
fi

mkdir -p "$LAYER_DEST"
cp "$LAYER_SRC/libkitvr_layer.so" "$LAYER_DEST/"
cp "$LAYER_SRC/kitvr_layer.json" "$LAYER_DEST/"

if [ ! -f "$BIN_DIR/vrcompositor.real" ]; then
    mv "$BIN_DIR/vrcompositor" "$BIN_DIR/vrcompositor.real"
fi
cp "$HERE/vrcompositor-wrapper.sh" "$BIN_DIR/vrcompositor"
chmod +x "$BIN_DIR/vrcompositor"

echo "Installed the KitVR layer into $LAYER_DEST"
echo "vrcompositor now loads it. Start SteamVR, then run:"
echo "  kitvr-server --source steamvr --stereo-sbs"
