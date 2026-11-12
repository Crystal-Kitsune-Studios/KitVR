#!/bin/sh
# Installed as <SteamVR>/bin/linux64/vrcompositor by linux/install.sh, with the original binary
# renamed to vrcompositor.real. SteamVR launches vrcompositor itself, so this is the only reliable
# place to inject the layer.
set -e
HERE=$(dirname "$0")
export VK_LAYER_PATH="$HERE/kitvr-layer${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
export VK_INSTANCE_LAYERS="VK_LAYER_CKS_kitvr${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
exec "$HERE/vrcompositor.real" "$@"
