# KitVR protocol v1 (draft)

Normative for both the PC server and the Quest client. All integers are little-endian.
The reference implementation of the parts marked "implemented" is `crates/proto`.

## Overview

| Channel | Transport | Port | Direction | Purpose |
|---|---|---|---|---|
| Discovery | mDNS | 5353 | server announces | Find the PC |
| Control | TCP | 9943 | both | Handshake, config, loss recovery, keepalive |
| Media | UDP | 9944 | video: server to client, tracking: client to server | Low-latency data |

## Discovery

The server advertises `_kitvr._tcp.local.` on the control port with TXT records
`ver=1` and `name=<host name>`. The client may also accept a manually typed IP address.

## Session flow

1. Client opens a TCP connection to the control port.
2. Both sides send `Hello`. If `version` differs, the side that cannot support the other's
   version sends `Bye` and closes.
3. Server sends `StreamConfig` (codec, resolution, fps, starting bitrate) and, if audio is
   enabled, `AudioConfig`.
4. Client configures its decoder, then starts sending tracking packets by UDP to the server's
   media port. The server learns the client's UDP address from the first packet and sends
   video there. This avoids any need for the client to announce its port.
5. Server streams video. Client sends `Ping` about once per second for RTT measurement.
6. On packet loss that it cannot hide, the client sends `KeyframeRequest`.
   If throughput is poor, it sends `BitrateRequest`.
7. Either side ends with `Bye`.

Joining a stream: the client discards frames until it receives one flagged as a keyframe, and
resets its reassembly state every time it receives a new server `Hello`, because frame ids
restart from 0 on a new session.

## Control messages (implemented)

Framing: `u32 length` (covers tag and body), `u8 tag`, body.
Maximum length is 65536. A decoder that sees `Truncated` should read more bytes and retry.

| Tag | Name | Body |
|---|---|---|
| 1 | Hello | `u16 version`, `u8 name_len`, `name` (UTF-8) |
| 2 | StreamConfig | `u8 codec` (1 H.264, 2 HEVC, 3 AV1), `u16 width`, `u16 height`, `u16 fps`, `u32 bitrate_kbps` |
| 3 | KeyframeRequest | `u32 last_good_frame` |
| 4 | BitrateRequest | `u32 bitrate_kbps` |
| 5 | Ping | `u64 ts_us` |
| 6 | Pong | `u64 ts_us` (echo of the Ping value) |
| 7 | Bye | empty |
| 8 | AudioConfig | `u32 sample_rate`, `u8 channels` (PC to headset, 0 = none), `u16 frame_samples` (per channel, per packet), `u8 mic_channels` (headset to PC, 0 = none) |

## Video packets (implemented)

24-byte header followed by `payload_len` bytes of encoded bitstream.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type, 1 = video |
| 1 | u8 | flags: bit0 keyframe, bit1 last slice of frame |
| 2 | u8 | slice_idx |
| 3 | u8 | slice_count (per frame) |
| 4 | u32 | frame_id |
| 8 | u64 | pose_timestamp_us |
| 16 | u16 | packet_idx (within the slice) |
| 18 | u16 | packet_count (within the slice) |
| 20 | u16 | payload_len |
| 22 | u16 | reserved, 0 |

Design notes:

- **Slices.** The server sends each encoder slice as soon as it is produced. Because the total
  size of later slices is unknown when the first is sent, `packet_idx` and `packet_count`
  are scoped to a slice, not a frame.
- **Pose timestamp.** The client stamps each tracking packet with its own monotonic clock. The
  server copies the timestamp of the pose a frame was rendered for into `pose_timestamp_us`,
  and the client uses it to reproject. No clock synchronization is needed because the value is
  only ever interpreted by the side that created it.
- **Payload size.** Default 1200 bytes per packet to stay under a 1500 byte MTU with IP/UDP headers.
- **Loss handling (v1).** If any packet of a slice is missing when the next frame's packets
  begin to arrive, the client drops that frame and sends `KeyframeRequest`. FEC is a later addition.

## Audio packets (implemented)

16-byte header followed by `payload_len` bytes of signed 16-bit little-endian PCM, interleaved.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type: 3 = audio to the headset, 4 = microphone to the PC |
| 1 | u8 | flags, 0 (bit 0 is reserved for Opus) |
| 2 | u16 | seq |
| 4 | u64 | timestamp_us (sender's monotonic clock) |
| 12 | u16 | payload_len |
| 14 | u16 | reserved, 0 |

Version 1 uses 48 kHz. PC audio is stereo in 240-sample (5 ms) packets. Microphone audio is mono
in 480-sample (10 ms) packets. The receiver keeps a small jitter buffer (about 30 ms), starts
playing once it is filled, and drops the oldest audio if it grows past about 80 ms. The server
converts whatever format the chosen PC device uses to 48 kHz stereo. PCM costs about 1.5 Mbps
downstream, which is small next to the video stream. Opus can replace it later using the flag bit.

## Tracking packets (implemented)

Client to server, UDP, sent every display frame (72 to 120 Hz). 136 bytes.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type, 2 = tracking |
| 1 | u8 | flags (reserved) |
| 2 | u16 | seq |
| 4 | u64 | timestamp_us (client monotonic clock) |
| 12 | 7 x f32 | head pose: position xyz, orientation quaternion xyzw |
| 40 | 48 bytes | left hand block |
| 88 | 48 bytes | right hand block |

Hand block: `u16 buttons`, `u16 flags` (bit0 = active), `f32 trigger`, `f32 grip`,
`f32 stick_x`, `f32 stick_y`, then pose as above (28 bytes).

The Quest client fills in the head pose and, when Touch controllers are active, sends their grip
poses, trigger, squeeze, thumbstick, and basic buttons in the hand blocks. SteamVR controller
support is experimental; desktop-mode pointer input remains separate.

## Security

Version 1 has no authentication or encryption. Use it on a trusted LAN only.
Planned for v2: PIN pairing shown in the headset, deriving a session key used to encrypt and
authenticate media packets (for example with ChaCha20-Poly1305).
