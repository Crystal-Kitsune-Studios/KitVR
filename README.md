# KitVR

Open-source PC-to-Quest streaming, in the spirit of Virtual Desktop.
Targets Quest 2, 3 and 3S. The PC server runs on Windows and Linux.

## Layout

```
crates/proto    wire protocol (no dependencies, fully unit tested)
crates/core     platform-neutral traits: CaptureSource, VideoEncoder, InputInjector
crates/server   PC server binary; src/platform/{windows,linux}.rs hold the OS backends
quest/          Android NDK + OpenXR client (see quest/README.md)
docs/PROTOCOL.md  normative protocol description
SECURITY.md     supported versions and how to report vulnerabilities
```

## Build

```
cargo build --workspace
cargo test --workspace
cargo run -p kitvr-server
```

The server currently prints which backends are missing and listens on the control
port (TCP 9943). It answers `Hello` and `Ping`, which is enough to test the Quest client's
networking before any video exists.

## Status

| Piece | State |
|---|---|
| Protocol: control messages, video packetizer | implemented, tests written |
| Protocol: tracking packets | specified, not implemented |
| Windows backends | stubs |
| Linux backends | stubs |
| Quest client | not started |

## Suggested next steps

1. Quest client, step one: decode a test HEVC stream (ffmpeg over UDP) with MediaCodec onto an
   OpenXR quad. This proves the headset side independently of the server.
2. Windows capture and encode, then wire it to `packetize_slice`.
3. Linux capture (PipeWire portal) and encode (VAAPI/NVENC) behind the same traits.
4. Tracking packets and input injection.
5. Latency work: pacing, keyframe recovery, adaptive bitrate.

## License

Not chosen yet. Decide before accepting contributions or copying code from other projects:
ALVR is MIT-licensed and WiVRn is GPL-licensed, so reading either for ideas is fine, but
copying code constrains your license.
