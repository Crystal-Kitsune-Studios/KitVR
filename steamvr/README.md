# KitVR SteamVR driver prototype

This experimental driver registers the Quest as a SteamVR HMD with left/right controllers and forwards head, grip-pose, and basic controller input from the KitVR server over loopback UDP port 9945. When Meta full-body tracking is available, it also registers waist, chest, knee, and foot generic trackers; assign their roles in SteamVR's tracker management UI.

## Install

1. Extract the `kitvr` directory from the archive.
2. Locate `vrpathreg` in the SteamVR installation (`bin/win64` on Windows or `bin/linux64` on Linux).
3. Run `vrpathreg adddriver <path-to-kitvr-directory>`.
4. Start the KitVR server and Quest client, then start or restart SteamVR.

Remove it with `vrpathreg removedriver <path-to-kitvr-directory>`.

This is not yet a complete wireless HMD driver: it does not transmit SteamVR stereo eye images to the Quest. SteamVR can enumerate the HMD and controllers and receive their tracking/input, but VR game frames will not yet be displayed in the headset. Lower-body trackers require the runtime's Meta full-body extension; older runtimes provide torso joints only or no body tracking.
