# KitVR on Linux

SteamVR on Linux will not give frames to an OpenVR driver, so KitVR gets them from the Vulkan
layer in `vklayer/` instead.

- `IVRDriverDirectModeComponent` is not implemented by SteamVR for Linux at all.
- `IVRVirtualDisplay`, which the SteamVR driver exposes on Windows, makes the Linux compositor fail
  to start with "Shared IPC Compositor Init Failed (303)". The driver therefore only returns that
  interface on Windows.

The layer is loaded into `vrcompositor` only, intercepts the swapchain it presents its mirror into,
copies each presented image to a host visible buffer, and publishes the pixels into a POSIX shared
memory ring. `kitvr-server --source steamvr` reads that ring and streams it.

```
SteamVR game -> vrcompositor -> [Vulkan layer] -> /dev/shm/kitvr-steamvr -> kitvr-server -> ffmpeg -> Quest
```

## Build and install

```
cmake -S linux/vklayer -B linux/vklayer/build
cmake --build linux/vklayer/build
./linux/install.sh
```

`install.sh` copies the layer to `<SteamVR>/bin/linux64/kitvr-layer`, renames the real
`vrcompositor` to `vrcompositor.real`, and installs `vrcompositor-wrapper.sh` as `vrcompositor` so
the layer is present in the process that actually composites. Set `STEAMVR_DIR` if SteamVR lives
somewhere other than `~/.steam/steam/steamapps/common/SteamVR`.

Undo it with `./linux/install.sh --uninstall`.

## Run

```
kitvr-server --source steamvr --stereo-sbs
```

`--stereo-sbs` treats the captured image as one eye per half. Drop it for a mono stream.
`--width` and `--height` scale the captured image, so you can stream below the capture resolution.

Both sides read `KITVR_STEAMVR_SHM` for the ring name, which defaults to `kitvr-steamvr`.

## Shared memory ring

The ring is a single frame slot. The producer writes the pixels, then bumps the frame id with a
release store, so a reader never sees a partially written frame under a fresh id.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `KVRF` (`0x4B565246`) |
| 4 | 4 | version, currently `1` |
| 8 | 4 | width in pixels |
| 12 | 4 | height in pixels |
| 16 | 4 | format, `0` = BGRA8, `1` = RGBA8 |
| 20 | 4 | stride in bytes, always `width * 4` |
| 24 | 4 | payload bytes, `stride * height` |
| 28 | 4 | reserved |
| 32 | 8 | frame id, bumped on every publish |
| 40 | 8 | `CLOCK_MONOTONIC` timestamp in nanoseconds |
| 48 | payload | pixels |

`crates/server/src/steamvr.rs` is the reference reader.

## Troubleshooting

- Nothing appears and the server says there is no ring: the layer is not loaded. Check that
  `vrcompositor` in the SteamVR bin directory is the wrapper script, then start SteamVR again.
- Set `KITVR_LAYER_DEBUG=1` before starting SteamVR for layer logs on stderr, which SteamVR writes
  to its own log.
- `ls -l /dev/shm/kitvr-steamvr` shows the ring and its size. The size is `48 + width * height * 4`.
- "unsupported swapchain format" in the layer log means vrcompositor is presenting a format the
  layer does not copy yet; add it to `shmFormat()` in `vklayer/kitvr_layer.cpp`.

## Limitations

- The layer waits for the readback to finish before returning from the present call, which costs a
  few milliseconds per frame. Doing the readback on the next present instead would trade latency
  for the same result.
- The captured image is whatever vrcompositor presents, including the mirror layout. Choose the
  eye layout in SteamVR's display mirror settings before streaming.
- The image is not reprojected. SteamVR renders at its own resolution and the headset displays it
  as a flat stereo pair, so head motion is tracked but not compensated.
