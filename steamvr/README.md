# KitVR SteamVR driver prototype

This experimental driver registers the Quest as a SteamVR HMD with left/right controllers and forwards head, grip-pose, basic controller input, and per-eye view geometry from the KitVR server over loopback UDP port 9945. When Meta full-body tracking is available, it also registers waist, chest, knee, and foot generic trackers; assign their roles in SteamVR's tracker management UI.

The controller profile supplies default legacy-game and SteamVR dashboard bindings. The dashboard bindings map the trigger or pinch to select, the primary button or grip to right-click, the joystick to scroll, and the left menu button to the dashboard.

## Install

1. Extract the `kitvr` directory from the archive.
2. Locate `vrpathreg` in the SteamVR installation (`bin/win64` on Windows or `bin/linux64` on Linux).
3. Run `vrpathreg adddriver <path-to-kitvr-directory>`.
4. Start the KitVR server and Quest client, then start or restart SteamVR.

Remove it with `vrpathreg removedriver <path-to-kitvr-directory>`.

## Frames on Windows

The driver exposes `IVRVirtualDisplay`. SteamVR's compositor hands it the finished, side-by-side
backbuffer (3664x1920 by default) as a shared D3D11 texture on every frame. The driver copies it to
a staging texture in `Present`, reads it back in `WaitForPresent`, and publishes the pixels into
the named shared memory section `Local\kitvr-steamvr`, with the same layout as the Linux ring
(see [../linux/README.md](../linux/README.md#shared-memory-ring)). Stream it with:

```
kitvr-server --source steamvr --stereo-sbs --encoder hevc_nvenc
```

The driver sets `Prop_GraphicsAdapterLuid_Uint64` to the first hardware GPU, because a shared
texture can only be opened on the adapter that created it. On a laptop with two GPUs, SteamVR then
renders on that one. Set `KITVR_STEAMVR_SHM` to the same name for SteamVR and the server to use a
different section name. Driver messages (for example an unsupported backbuffer format) appear in
SteamVR's `vrserver.txt` log with a `KitVR:` prefix.

## Frames on Linux

On Linux the Windows frame path does not exist, because SteamVR for Linux does not start the
compositor when a driver exposes `IVRVirtualDisplay` and does not implement
`IVRDriverDirectModeComponent` at all. The driver only returns `IVRVirtualDisplay` on Windows; on
Linux, frames are captured by the Vulkan layer described in [../linux/README.md](../linux/README.md).

## Limitations

The image is the compositor's output, so it is not reprojected for head motion that happens
during streaming. Lower-body trackers require the runtime's Meta full-body extension; older runtimes
provide torso joints only or no body tracking.
