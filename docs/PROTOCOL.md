# KitVR protocol v2

Normative for both the PC server and the Quest client. Protocol integer fields are
little-endian; the AES-GCM nonce encodes its sequence number in big-endian order.
The reference implementation is `crates/proto`.

## Overview

| Channel | Transport | Port | Direction | Purpose |
|---|---|---|---|---|
| Discovery | mDNS | 5353 | server announces | Find the PC |
| Control | TCP | 9943 | both | Handshake, config, loss recovery, keepalive |
| Media | UDP | 9944 | video: server to client, tracking/body/views: client to server | Low-latency data |

## Discovery

The server advertises `_kitvr._tcp.local.` on the control port with TXT records
`ver=2` and `name=<host name>`. The client may also accept a manually typed IP address.
Discovery metadata is not authenticated.

## Session flow

1. Client opens a TCP connection to the control port and sends a 68-byte pairing request:
   `KVH2`, a 32-byte random client nonce, and a 32-byte HMAC-SHA-256 proof. The server
   verifies the proof using the pairing key, then replies with `KVS2`, a random 32-byte
   server nonce, and its 32-byte HMAC-SHA-256 proof. Invalid or unsupported handshakes
   are closed; there is no unencrypted fallback.
2. Both sides derive four separate AES-256 keys with HKDF-SHA-256 using the pairing key,
   both nonces, and direction/channel-specific labels: client-to-server and
   server-to-client, each for control and media.
3. Control messages use the framing described below, but every record is AES-256-GCM
   authenticated and encrypted. The sequence number is per direction, is included in
   authenticated data, and must be the next expected value.
4. Every UDP media packet is wrapped in an authenticated `KVE2` envelope. Its header and
   sequence number are authenticated, and the original media packet is encrypted. The
   server learns the client's UDP address from the first valid tracking packet from the
   TCP peer's IP; subsequent packets must match that address. A 64-packet replay window
   permits reordering while rejecting duplicate and stale datagrams.
5. Server sends `StreamConfig` (codec, resolution, fps, starting bitrate) and, if audio is
   enabled, `AudioConfig`. Client configures its decoder and begins sending tracking by UDP.
6. Server streams video. Client sends `Ping` about once per second for RTT measurement.
   The server closes a session if it receives no complete control message for 15 seconds.
7. On packet loss that it cannot hide, the client sends `KeyframeRequest`.
   If throughput is poor, it sends `BitrateRequest`.
8. Either side ends with `Bye`.

Joining a stream: the client discards frames until it receives one flagged as a keyframe, and
resets its reassembly state every time it receives a new server `Hello`, because frame ids
restart from 0 on a new session.

## Encrypted control records

Each TCP record is `u32 length`, `u64 sequence`, then AES-256-GCM ciphertext and 16-byte
tag. Length covers the sequence and ciphertext/tag; it is at most 65536. The nonce is a
zero 32-bit prefix followed by the big-endian sequence number. Associated data is the
UTF-8 string `KitVR v2 control` followed by the little-endian sequence number. After
decryption, the plaintext is a control message: `u8 tag`, body.

## Control messages

| Tag | Name | Body |
|---|---|---|
| 1 | Hello | `u16 version`, `u8 name_len`, `name` (UTF-8) |
| 2 | StreamConfig | `u8 codec` (1 H.264, 2 HEVC, 3 AV1), `u16 width`, `u16 height`, `u16 fps`, `u32 bitrate_kbps`, optional `u8 layout` |
| 3 | KeyframeRequest | `u32 last_good_frame` |
| 4 | BitrateRequest | `u32 bitrate_kbps` |
| 5 | Ping | `u64 ts_us` |
| 6 | Pong | `u64 ts_us` (echo of the Ping value) |
| 7 | Bye | empty |
| 8 | AudioConfig | `u32 sample_rate`, `u8 channels` (PC to headset, 0 = none), `u16 frame_samples` (per channel, per packet), `u8 mic_channels` (headset to PC, 0 = none) |

## Encrypted media datagrams

Each UDP datagram is `KVE2`, `u64 sequence` (little-endian), then AES-256-GCM ciphertext
and a 16-byte tag. The nonce is a zero 32-bit prefix followed by the big-endian sequence
number; the complete 12-byte header is associated data. The plaintext is one existing
video, audio, tracking, body-tracking, or view-configuration packet. Sequence numbers are
independent for each sending direction and session.

## Video packets

The optional `StreamConfig.layout` byte is `0` or absent for mono, and `1` for side-by-side
stereo. For stereo, `width` is the full encoded image width; the client displays the left and
right halves to their matching eyes. Existing readers ignore the trailing byte, and new readers
default to mono when it is absent.

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
- **Pose timestamp.** The client stamps each tracking packet with its own monotonic clock. A
  future VR frame source must copy the timestamp of the pose the frame was rendered for into
  `pose_timestamp_us`, so the client can use it for reprojection. No clock synchronization is
  needed because the value is only interpreted by the side that created it. A value of zero
  means that the render pose is unknown; desktop, test-pattern and video-file sources always use
  zero. A recently received tracking pose is not necessarily the pose used to render a frame.
  The current client does not yet reproject video frames.
- **Payload size.** Default 1200 bytes per packet to stay under a 1500 byte MTU with IP/UDP headers.
- **Loss handling (v1).** If any packet of a slice is missing when the next frame's packets
  begin to arrive, the client drops that frame and sends `KeyframeRequest`. The client also drops
  in-progress frames with inconsistent packet counts, keyframe flags, pose timestamps, or
  conflicting duplicate packet contents. FEC is a later addition.

## Audio packets

16-byte header followed by `payload_len` bytes of signed 16-bit little-endian PCM, interleaved.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type: 3 = audio to the headset, 4 = microphone to the PC |
| 1 | u8 | flags, 0 (bit 0 is reserved for Opus) |
| 2 | u16 | seq |
| 4 | u64 | timestamp_us (sender's monotonic clock) |
| 12 | u16 | payload_len |
| 14 | u16 | reserved, 0 |

Version 2 uses 48 kHz. PC audio is stereo in 240-sample (5 ms) packets. Microphone audio is mono
in 480-sample (10 ms) packets. The receiver keeps a small jitter buffer (about 30 ms), starts
playing once it is filled, and drops the oldest audio if it grows past about 80 ms. The server
converts whatever format the chosen PC device uses to 48 kHz stereo. PCM costs about 1.5 Mbps
downstream, which is small next to the video stream. Opus can replace it later using the flag bit.

## Tracking packets

Client to server, UDP, sent every display frame (72 to 120 Hz). The original packet is 136
bytes; an optional aim-pose extension makes it 192 bytes.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type, 2 = tracking |
| 1 | u8 | flags (bit 0 = aim-pose extension) |
| 2 | u16 | seq |
| 4 | u64 | timestamp_us (client monotonic clock) |
| 12 | 7 x f32 | head pose: position xyz, orientation quaternion xyzw |
| 40 | 48 bytes | left hand block |
| 88 | 48 bytes | right hand block |
| 136 | 7 x f32 | left aim pose relative to left grip (optional) |
| 164 | 7 x f32 | right aim pose relative to right grip (optional) |

Hand block: `u16 buttons`, `u16 flags` (bit0 = active, bit1 = aim pose valid), `f32 trigger`, `f32 grip`,
`f32 stick_x`, `f32 stick_y`, then pose as above (28 bytes).

The SteamVR driver ignores duplicate and older sequence numbers, using 16-bit wraparound
ordering. It resets the sequence after tracking times out so a restarted client can reconnect.

The Quest client fills in the head pose and, when Touch controllers are active, sends their grip
poses, trigger, squeeze, thumbstick, and basic buttons in the hand blocks. Otherwise, when hand
tracking is available, it sends the palm pose and derives trigger and grip from finger positions.
Head, hand and body poses share the floor-relative OpenXR stage space when available; otherwise
the client logs a warning and uses the eye-level local space.
This fallback is independent of controller-action availability; missing finger positions release
the affected gestures without discarding a valid palm pose. Pinch does not synthesize the primary
button. SteamVR dashboard bindings map trigger/pinch to left-click and grip/grab to right-click.
The dashboard pointer uses the controller's OpenXR aim pose when available, otherwise the grip
pose. Older clients without the extension retain the grip-pose fallback.
SteamVR controller support is
experimental; desktop-mode pointer input remains separate.

## Body-tracking packets (optional)

When the OpenXR runtime exposes Meta's full-body joint set, the Quest may send a 184-byte body
packet alongside normal tracking packets. The server validates and forwards it to the local
SteamVR driver on UDP port 9945. When desktop avatar tracking is enabled, head/hand packets and
body packets from the established media client are also forwarded to UDP loopback port 9946.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type, 5 = body tracking |
| 1 | u8 | packet version, 1 |
| 2 | u16 | sequence |
| 4 | u64 | timestamp_us |
| 12 | u16 | active joint mask |
| 14 | u16 | reserved, 0 |
| 16 | 6 x 28 bytes | hips, chest, left knee, right knee, left ankle, right ankle poses |

Each pose is position xyz and orientation quaternion xyzw as seven little-endian `f32` values.
Only joints whose active-mask bit is set are valid. Runtimes without Meta's full-body extension
may report torso joints only; unsupported lower-body points remain inactive.
Body packets use their own sequence counter and timeout, independent of head and hand packets.

## View configuration packets (optional)

The client periodically sends the actual stereo view geometry after tracking starts. The server
forwards it to the local SteamVR driver. Older clients omit it, leaving the driver's defaults.
Each packet is 92 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type, 6 = view configuration |
| 1 | u8 | packet version, 1 |
| 2 | u16 | reserved, 0 |
| 4 | 2 x 44 bytes | left and right eye blocks |

Each eye block contains four `f32` projection tangents in OpenVR order (left, right, top,
bottom), followed by an eye-to-head pose (position xyz, quaternion xyzw). OpenXR's up/down
angles are negated when converted to OpenVR's vertical tangent convention.
On Linux the driver mirrors the vertical asymmetry when configuring SteamVR's captured display,
so its upright compositor output matches the headset's OpenXR frustum. The packet format itself
remains platform-independent.

## Security

The pairing key is 32 cryptographically random bytes, represented as 64 hexadecimal
characters. The server creates it on first run, saves it in
`%APPDATA%/kitvr/pairing.key` on Windows or `$XDG_CONFIG_HOME/kitvr/pairing.key`
(`~/.config/kitvr/pairing.key` by default) on Unix, and prints it once for entry in the
Quest launcher. Use
`kitvr-server --show-pairing-key` to display an existing key again. On Unix, the key file
must be owner-readable/writable only. `KITVR_PAIRING_KEY_FILE` overrides its location.
The Quest launcher stores the key in app-private preferences; it is a shared secret and
must not be disclosed.

HMAC-SHA-256 authenticates the pairing handshake; HKDF-SHA-256 derives session keys; and
AES-256-GCM encrypts and authenticates control and UDP media packets. SHA-512 is a hash,
not an encryption algorithm, and is not used by this protocol. Packet timing, sizes,
network addresses, and unauthenticated discovery metadata remain observable. This version
rejects protocol-v1 clients and has no insecure fallback.
