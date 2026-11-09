//! TCP control channel. Each message is framed as:
//!   u32 LE  length of (tag + body)
//!   u8      tag
//!   ...     body (little-endian fields)

use crate::DecodeError;

pub const MAX_CONTROL_LEN: u32 = 64 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Codec {
    H264 = 1,
    Hevc = 2,
    Av1 = 3,
}

impl Codec {
    fn from_u8(v: u8) -> Result<Self, DecodeError> {
        match v {
            1 => Ok(Codec::H264),
            2 => Ok(Codec::Hevc),
            3 => Ok(Codec::Av1),
            other => Err(DecodeError::BadType(other)),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Hello { version: u16, name: String },
    StreamConfig { codec: Codec, width: u16, height: u16, fps: u16, bitrate_kbps: u32 },
    /// Audio format. `channels` is the PC-to-headset stream (0 = none), `mic_channels` is the
    /// headset-to-PC microphone stream (0 = none). Samples are 16 bit PCM.
    AudioConfig { sample_rate: u32, channels: u8, frame_samples: u16, mic_channels: u8 },
    /// Client lost data; `last_good_frame` is the last frame_id it fully decoded.
    KeyframeRequest { last_good_frame: u32 },
    BitrateRequest { bitrate_kbps: u32 },
    Ping { ts_us: u64 },
    Pong { ts_us: u64 },
    Bye,
}

const TAG_HELLO: u8 = 1;
const TAG_STREAM_CONFIG: u8 = 2;
const TAG_KEYFRAME_REQUEST: u8 = 3;
const TAG_BITRATE_REQUEST: u8 = 4;
const TAG_PING: u8 = 5;
const TAG_PONG: u8 = 6;
const TAG_BYE: u8 = 7;
const TAG_AUDIO_CONFIG: u8 = 8;

pub fn encode_message(msg: &Message) -> Vec<u8> {
    let mut body = Vec::new();
    let tag = match msg {
        Message::Hello { version, name } => {
            body.extend_from_slice(&version.to_le_bytes());
            let bytes = name.as_bytes();
            let n = bytes.len().min(255);
            body.push(n as u8);
            body.extend_from_slice(&bytes[..n]);
            TAG_HELLO
        }
        Message::StreamConfig { codec, width, height, fps, bitrate_kbps } => {
            body.push(*codec as u8);
            body.extend_from_slice(&width.to_le_bytes());
            body.extend_from_slice(&height.to_le_bytes());
            body.extend_from_slice(&fps.to_le_bytes());
            body.extend_from_slice(&bitrate_kbps.to_le_bytes());
            TAG_STREAM_CONFIG
        }
        Message::AudioConfig { sample_rate, channels, frame_samples, mic_channels } => {
            body.extend_from_slice(&sample_rate.to_le_bytes());
            body.push(*channels);
            body.extend_from_slice(&frame_samples.to_le_bytes());
            body.push(*mic_channels);
            TAG_AUDIO_CONFIG
        }
        Message::KeyframeRequest { last_good_frame } => {
            body.extend_from_slice(&last_good_frame.to_le_bytes());
            TAG_KEYFRAME_REQUEST
        }
        Message::BitrateRequest { bitrate_kbps } => {
            body.extend_from_slice(&bitrate_kbps.to_le_bytes());
            TAG_BITRATE_REQUEST
        }
        Message::Ping { ts_us } => {
            body.extend_from_slice(&ts_us.to_le_bytes());
            TAG_PING
        }
        Message::Pong { ts_us } => {
            body.extend_from_slice(&ts_us.to_le_bytes());
            TAG_PONG
        }
        Message::Bye => TAG_BYE,
    };
    let mut out = Vec::with_capacity(5 + body.len());
    out.extend_from_slice(&((body.len() as u32) + 1).to_le_bytes());
    out.push(tag);
    out.extend_from_slice(&body);
    out
}

struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], DecodeError> {
        if self.buf.len() < self.pos + n {
            return Err(DecodeError::BadLength);
        }
        let s = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }
    fn u8(&mut self) -> Result<u8, DecodeError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, DecodeError> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32, DecodeError> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64, DecodeError> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
}

/// Decodes one message from the front of `buf`.
/// Returns the message and the number of bytes consumed.
/// `Err(Truncated)` means "read more bytes and call again".
pub fn decode_message(buf: &[u8]) -> Result<(Message, usize), DecodeError> {
    if buf.len() < 4 {
        return Err(DecodeError::Truncated);
    }
    let len = u32::from_le_bytes(buf[0..4].try_into().unwrap());
    if len == 0 || len > MAX_CONTROL_LEN {
        return Err(DecodeError::BadLength);
    }
    let total = 4 + len as usize;
    if buf.len() < total {
        return Err(DecodeError::Truncated);
    }
    let tag = buf[4];
    let mut r = Reader { buf: &buf[5..total], pos: 0 };
    let msg = match tag {
        TAG_HELLO => {
            let version = r.u16()?;
            let n = r.u8()? as usize;
            let name = std::str::from_utf8(r.take(n)?).map_err(|_| DecodeError::BadUtf8)?.to_string();
            Message::Hello { version, name }
        }
        TAG_STREAM_CONFIG => Message::StreamConfig {
            codec: Codec::from_u8(r.u8()?)?,
            width: r.u16()?,
            height: r.u16()?,
            fps: r.u16()?,
            bitrate_kbps: r.u32()?,
        },
        TAG_AUDIO_CONFIG => Message::AudioConfig {
            sample_rate: r.u32()?,
            channels: r.u8()?,
            frame_samples: r.u16()?,
            mic_channels: r.u8()?,
        },
        TAG_KEYFRAME_REQUEST => Message::KeyframeRequest { last_good_frame: r.u32()? },
        TAG_BITRATE_REQUEST => Message::BitrateRequest { bitrate_kbps: r.u32()? },
        TAG_PING => Message::Ping { ts_us: r.u64()? },
        TAG_PONG => Message::Pong { ts_us: r.u64()? },
        TAG_BYE => Message::Bye,
        other => return Err(DecodeError::BadType(other)),
    };
    Ok((msg, total))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn samples() -> Vec<Message> {
        vec![
            Message::Hello { version: 1, name: "Quest 3".to_string() },
            Message::StreamConfig { codec: Codec::Hevc, width: 1920, height: 1080, fps: 90, bitrate_kbps: 80_000 },
            Message::AudioConfig { sample_rate: 48_000, channels: 2, frame_samples: 240, mic_channels: 1 },
            Message::KeyframeRequest { last_good_frame: 1234 },
            Message::BitrateRequest { bitrate_kbps: 50_000 },
            Message::Ping { ts_us: 1 },
            Message::Pong { ts_us: 2 },
            Message::Bye,
        ]
    }

    #[test]
    fn roundtrip_all() {
        for m in samples() {
            let bytes = encode_message(&m);
            let (decoded, used) = decode_message(&bytes).unwrap();
            assert_eq!(decoded, m);
            assert_eq!(used, bytes.len());
        }
    }

    #[test]
    fn stream_of_messages() {
        let mut stream = Vec::new();
        for m in samples() {
            stream.extend_from_slice(&encode_message(&m));
        }
        let mut out = Vec::new();
        while !stream.is_empty() {
            let (m, used) = decode_message(&stream).unwrap();
            out.push(m);
            stream.drain(..used);
        }
        assert_eq!(out, samples());
    }

    #[test]
    fn truncated_and_bad() {
        let bytes = encode_message(&Message::Ping { ts_us: 5 });
        assert_eq!(decode_message(&bytes[..bytes.len() - 1]), Err(DecodeError::Truncated));
        assert_eq!(decode_message(&[0, 0, 0, 0]), Err(DecodeError::BadLength));
        assert_eq!(decode_message(&[1, 0, 0, 0, 99]), Err(DecodeError::BadType(99)));
    }
}
