#!/bin/sh
# Installed as <SteamVR>/bin/linux64/vrcompositor by linux/install.sh, with the original binary
# renamed to vrcompositor.real. SteamVR launches vrcompositor itself, so this is the only reliable
# place to inject the layer.
#
# SteamVR for Linux only renders to a display it drives directly. The layer provides a virtual one
# through VK_KHR_display; hiding the Wayland socket makes vrcompositor look for it there instead of
# asking the Wayland compositor for a DRM lease, which a streamed headset cannot satisfy.
set -e
HERE=$(dirname "$0")
export VK_LAYER_PATH="$HERE/kitvr-layer${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
export VK_INSTANCE_LAYERS="VK_LAYER_CKS_kitvr${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
export KITVR_VIRTUAL_DISPLAY=1
unset WAYLAND_DISPLAY
exec "$HERE/vrcompositor.real" "$@"
