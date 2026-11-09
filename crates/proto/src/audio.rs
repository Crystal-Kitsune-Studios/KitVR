//! UDP audio packets. 16 byte header followed by PCM, little-endian.
//!
//!   0   u8   type (3 = audio to the headset, 4 = microphone to the PC)
//!   1   u8   flags (0). Bit 0 will mean "Opus" once a codec is added.
//!   2   u16  seq
//!   4   u64  timestamp_us (sender's monotonic clock)
//!   12  u16  payload_len
//!   14  u16  reserved, 0
//!
//! Payload: signed 16 bit PCM, interleaved. The format comes from `AudioConfig`.

use crate::DecodeError;

pub const PACKET_TYPE_AUDIO_DOWN: u8 = 3;
pub const PACKET_TYPE_AUDIO_UP: u8 = 4;
pub const AUDIO_HEADER_LEN: usize = 16;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AudioHeader {
    pub packet_type: u8,
    pub seq: u16,
    pub timestamp_us: u64,
    pub payload_len: u16,
}

impl AudioHeader {
    pub fn write_to(&self, out: &mut Vec<u8>) {
        out.push(self.packet_type);
        out.push(0);
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.timestamp_us.to_le_bytes());
        out.extend_from_slice(&self.payload_len.to_le_bytes());
        out.extend_from_slice(&[0, 0]);
    }

    pub fn parse(buf: &[u8]) -> Result<(Self, &[u8]), DecodeError> {
        if buf.len() < AUDIO_HEADER_LEN {
            return Err(DecodeError::Truncated);
        }
        let t = buf[0];
        if t != PACKET_TYPE_AUDIO_DOWN && t != PACKET_TYPE_AUDIO_UP {
            return Err(DecodeError::BadType(t));
        }
        let h = Self {
            packet_type: t,
            seq: u16::from_le_bytes(buf[2..4].try_into().unwrap()),
            timestamp_us: u64::from_le_bytes(buf[4..12].try_into().unwrap()),
            payload_len: u16::from_le_bytes(buf[12..14].try_into().unwrap()),
        };
        let end = AUDIO_HEADER_LEN + h.payload_len as usize;
        if buf.len() < end {
            return Err(DecodeError::Truncated);
        }
        Ok((h, &buf[AUDIO_HEADER_LEN..end]))
    }
}

/// Builds a complete packet from interleaved samples.
pub fn build_packet(packet_type: u8, seq: u16, timestamp_us: u64, pcm: &[i16]) -> Vec<u8> {
    let h = AudioHeader { packet_type, seq, timestamp_us, payload_len: (pcm.len() * 2) as u16 };
    let mut out = Vec::with_capacity(AUDIO_HEADER_LEN + pcm.len() * 2);
    h.write_to(&mut out);
    for s in pcm {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

pub fn pcm_from_payload(payload: &[u8]) -> Vec<i16> {
    payload.chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let pcm: Vec<i16> = vec![0, 1, -1, i16::MAX, i16::MIN, 1234];
        let pkt = build_packet(PACKET_TYPE_AUDIO_DOWN, 77, 5_000_000, &pcm);
        assert_eq!(pkt.len(), AUDIO_HEADER_LEN + pcm.len() * 2);
        let (h, payload) = AudioHeader::parse(&pkt).unwrap();
        assert_eq!(h.seq, 77);
        assert_eq!(h.timestamp_us, 5_000_000);
        assert_eq!(h.packet_type, PACKET_TYPE_AUDIO_DOWN);
        assert_eq!(pcm_from_payload(payload), pcm);
    }

    #[test]
    fn rejects_bad_input() {
        assert_eq!(AudioHeader::parse(&[3u8; 4]), Err(DecodeError::Truncated));
        let mut buf = vec![0u8; AUDIO_HEADER_LEN];
        buf[0] = 1;
        assert_eq!(AudioHeader::parse(&buf), Err(DecodeError::BadType(1)));
        buf[0] = PACKET_TYPE_AUDIO_UP;
        buf[12] = 8; // claims 8 payload bytes that are missing
        assert_eq!(AudioHeader::parse(&buf), Err(DecodeError::Truncated));
    }
}
