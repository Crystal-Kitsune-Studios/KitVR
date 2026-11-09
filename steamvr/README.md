# KitVR SteamVR driver prototype

This experimental driver registers the Quest as a tracked SteamVR HMD and forwards head poses from the KitVR server over loopback UDP port 9945.

## Install

1. Extract the `kitvr` directory from the archive.
2. Locate `vrpathreg` in the SteamVR installation (`bin/win64` on Windows or `bin/linux64` on Linux).
3. Run `vrpathreg adddriver <path-to-kitvr-directory>`.
4. Start the KitVR server and Quest client, then start or restart SteamVR.

Remove it with `vrpathreg removedriver <path-to-kitvr-directory>`.

This is not yet a complete wireless HMD driver: it does not transmit SteamVR stereo eye images to the Quest, and it does not expose controllers. SteamVR may enumerate the device and receive tracking, but VR game frames will not yet be displayed in the headset.
