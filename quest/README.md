# Quest client

Native Android app (C++ via the NDK) using OpenXR and the Meta OpenXR mobile SDK.
Works on Quest 2, 3 and 3S from one APK.

## Planned structure

```
quest/
  app/                Gradle project, AndroidManifest, native-activity entry
  cpp/
    xr/               OpenXR session, swapchains, frame loop, quad layer
    net/              control TCP client, UDP receive and tracking send
    video/            packet reassembly, AMediaCodec decoder, SurfaceTexture
    proto/            C++ port of crates/proto (or generated from it)
```

## Key points

- Decode with `AMediaCodec`, output to a Surface backed by a `SurfaceTexture`, sample it as
  `GL_TEXTURE_EXTERNAL_OES` and draw it on an OpenXR quad layer.
- Set the low-latency decoder key and feed input buffers as slices arrive.
- HEVC is the common codec for all three headsets. AV1 hardware decode is Quest 3 and 3S only.
- Use `adb` over Wi-Fi for development. Developer mode must be enabled on the headset.
- Stamp every tracking packet with `xrConvertTimeToTimespecTime`-based monotonic microseconds
  and keep a ring buffer of recent poses keyed by timestamp for reprojection.
