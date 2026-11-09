# KitVR

Open-source PC-to-Quest streaming, in the spirit of Virtual Desktop.
Targets Quest 2, 3 and 3S. The PC server runs on Windows and Linux.

## Layout

```
crates/proto    wire protocol (no dependencies, fully unit tested)
crates/core     platform-neutral traits: CaptureSource, VideoEncoder, InputInjector
crates/server   PC server binary; bridge.rs streams via ffmpeg, platform/ holds native backends
quest/          Android NDK + OpenXR client (see quest/README.md)
docs/PROTOCOL.md  normative protocol description
SECURITY.md     supported versions and how to report vulnerabilities
```

## Quick start (test pattern, no capture needed)

You need Rust, ffmpeg on your PATH, and a Quest with developer mode enabled.

1. On the PC (Windows or Linux), allow TCP 9943 and UDP 9944 through the firewall, then:

   ```
   cargo test --workspace
   cargo run -p kitvr-server --release
   ```

   This streams ffmpeg's test pattern with software HEVC encoding, so it works on any machine.
   Set `KITVR_FFMPEG` if ffmpeg is not on PATH.

2. Build and install the Quest app, then launch it with your PC's IP address.
   See [quest/README.md](quest/README.md).

3. Stream your desktop instead of the test pattern, with a hardware encoder:

   ```
   cargo run -p kitvr-server --release -- --source desktop --encoder hevc_nvenc
   ```

   Encoders: `hevc_nvenc` (NVIDIA), `hevc_amf` (AMD, Windows), `hevc_qsv` (Intel),
   `hevc_vaapi` (AMD/Intel, Linux), `libx265` (CPU). Desktop capture uses `gdigrab` on Windows
   and `x11grab` on Linux (X11 or XWayland only).

## How the server works today

The server spawns ffmpeg to capture and encode, splits its HEVC output into frames, and sends
them with the KitVR packets. It works on both OSes with one code path. Known costs:

- About one extra frame of latency, because a frame is only known to be complete when the next begins.
- Keyframe requests cannot be honored. The stream uses a fixed GOP (`--gop`, default 60 frames),
  so recovery after packet loss takes up to one second.
- No input injection yet, and no Wayland capture.

Replacing the bridge with the native backends in `crates/server/src/platform/` removes these.

## Status

| Piece | State |
|---|---|
| Protocol: control, video, tracking | implemented in Rust and C++ |
| Server: ffmpeg bridge (Windows and Linux) | implemented, untested on hardware |
| Server: native capture, encode, input | stubs |
| Quest client: OpenXR, MediaCodec, networking | implemented, untested on hardware |
| Controller and hand input | not started |
| Authentication and encryption | not started |

Nothing here has been compiled or run yet. Expect to fix a few compile errors and
device-specific issues on first build.

## Next steps

1. Get the test pattern showing in the headset.
2. Desktop capture with a hardware encoder, then measure latency.
3. Controller pointer to mouse input.
4. Native capture and encode backends (removes the bridge's limits).
5. Pairing and encrypted media.

## License

Not chosen yet. Decide before accepting contributions or copying code from other projects:
ALVR is MIT-licensed and WiVRn is GPL-licensed, so reading either for ideas is fine, but
copying code constrains your license.
