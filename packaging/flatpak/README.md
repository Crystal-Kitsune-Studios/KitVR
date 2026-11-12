# KitVR as a Flatpak

`com.crystalkitsune.KitVR.yaml` builds the desktop app, the server, and everything the server
shells out to. It is aimed at immutable distros such as Bazzite, where Flatpak is the normal way to
install a GUI app.

## Build and install

```
flatpak install flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08 org.freedesktop.Sdk.Extension.rust-stable//24.08
flatpak-builder --user --install --force-clean build-dir packaging/flatpak/com.crystalkitsune.KitVR.yaml
flatpak run com.crystalkitsune.KitVR
```

For a single-file bundle:

```
flatpak-builder --repo=repo --force-clean build-dir packaging/flatpak/com.crystalkitsune.KitVR.yaml
flatpak build-bundle repo KitVR-linux-x86_64.flatpak com.crystalkitsune.KitVR
```

## What the manifest does

- Builds x265 and ffmpeg into `/app`. KitVR runs the `ffmpeg` binary, and the `ffmpeg-full`
  runtime extension only ships libraries, so ffmpeg is built here. `KITVR_FFMPEG` is pointed at it
  by `kitvr-launch.sh`.
- Installs `kitvr-desktop`, `kitvr-server`, and `kitvr-updater` into `/app/bin`, with
  `kitvr-desktop` as a wrapper that execs `kitvr-desktop.real`.
- Grants `--share=ipc`, which is what lets the server read `/dev/shm/kitvr-steamvr` written by a
  different sandbox.

## SteamVR inside a sandbox

The Vulkan layer has to run inside the process that composites, so it must be installed into the
SteamVR tree that Steam actually uses:

- Steam installed as a Flatpak: `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR`.
- Steam installed natively: `~/.steam/steam/steamapps/common/SteamVR`.

That directory is writable from the host, so `linux/install.sh` works either way. Set `STEAMVR_DIR`
to point it at the Flatpak path.

The part that needs testing is the handoff. Steam's sandbox writes the ring, KitVR's sandbox reads
it, and both need `--share=ipc` for that to work. Check it with:

```
flatpak run --command=sh com.crystalkitsune.KitVR -c 'ls -l /dev/shm/kitvr-steamvr'
```

If the ring is not visible from inside the KitVR sandbox, point both sides at a directory they can
both see instead. Both the layer and the server read `KITVR_STEAMVR_SHM_DIR` (default `/dev/shm`),
so for example a host path added with `--filesystem=xdg-run/kitvr:create` on both sandboxes works.

## Known limits

- Hardware encoding is not wired up. The manifest builds ffmpeg with libx265 only, so the server
  runs on the CPU encoder. Adding `--enable-vaapi` works on AMD and Intel; NVENC needs CUDA headers,
  which the Freedesktop SDK does not ship.
- "Get update" in the desktop app opens the GitHub release page instead of self-updating, because
  `/app` is read-only. Update the Flatpak itself.
- The Vulkan layer is not part of the Flatpak. It goes into the SteamVR install, as above.
