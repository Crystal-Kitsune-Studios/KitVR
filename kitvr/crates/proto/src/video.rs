//! UDP video packets. All integers are little-endian.
//!
//! Header layout (24 bytes):
//!   0  u8   packet type (1 = video)
//!   1  u8   flags (bit0 keyframe, bit1 last slice of frame)
//!   2  u8   slice_idx
//!   3  u8   slice_count (per frame)
//!   4  u32  frame_id
//!   8  u64  pose_timestamp_us (pose the frame was rendered for, client clock domain)
//!   16 u16  packet_idx   (within the slice)
//!   18 u16  packet_count (within the slice)
//!   20 u16  payload_len
//!   22 u16  reserved, must be 0

use crate::DecodeError;

pub const PACKET_TYPE_VIDEO: u8 = 1;
pub const VIDEO_HEADER_LEN: usize = 24;

pub mod flags {
    pub const KEYFRAME: u8 = 1 << 0;
    pub const LAST_SLICE: u8 = 1 << 1;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VideoHeader {
    pub flags: u8,
    pub slice_idx: u8,
    pub slice_count: u8,
    pub frame_id: u32,
    pub pose_timestamp_us: u64,
    pub packet_idx: u16,
    pub packet_count: u16,
    pub payload_len: u16,
}

impl VideoHeader {
    pub fn write_to(&self, out: &mut Vec<u8>) {
        out.push(PACKET_TYPE_VIDEO);
        out.push(self.flags);
        out.push(self.slice_idx);
        out.push(self.slice_count);
        out.extend_from_slice(&self.frame_id.to_le_bytes());
        out.extend_from_slice(&self.pose_timestamp_us.to_le_bytes());
        out.extend_from_slice(&self.packet_idx.to_le_bytes());
        out.extend_from_slice(&self.packet_count.to_le_bytes());
        out.extend_from_slice(&self.payload_len.to_le_bytes());
        out.extend_from_slice(&[0, 0]);
    }

    /// Parses one packet, returning the header and a slice of its payload.
    pub fn parse(buf: &[u8]) -> Result<(Self, &[u8]), DecodeError> {
        if buf.len() < VIDEO_HEADER_LEN {
            return Err(DecodeError::Truncated);
        }
        if buf[0] != PACKET_TYPE_VIDEO {
            return Err(DecodeError::BadType(buf[0]));
        }
        let h = Self {
            flags: buf[1],
            slice_idx: buf[2],
            slice_count: buf[3],
            frame_id: u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            pose_timestamp_us: u64::from_le_bytes(buf[8..16].try_into().unwrap()),
            packet_idx: u16::from_le_bytes(buf[16..18].try_into().unwrap()),
            packet_count: u16::from_le_bytes(buf[18..20].try_into().unwrap()),
            payload_len: u16::from_le_bytes(buf[20..22].try_into().unwrap()),
        };
        let end = VIDEO_HEADER_LEN + h.payload_len as usize;
        if buf.len() < end {
            return Err(DecodeError::Truncated);
        }
        Ok((h, &buf[VIDEO_HEADER_LEN..end]))
    }
}

#[derive(Debug, Clone, Copy)]
pub struct SliceInfo {
    pub frame_id: u32,
    pub slice_idx: u8,
    pub slice_count: u8,
    pub keyframe: bool,
    pub pose_timestamp_us: u64,
}

/// Splits one encoded slice into ready-to-send UDP packets (header + payload).
pub fn packetize_slice(info: &SliceInfo, data: &[u8], max_payload: usize) -> Vec<Vec<u8>> {
    assert!(max_payload > 0 && max_payload <= u16::MAX as usize);
    let count = ((data.len() + max_payload - 1) / max_payload).max(1);
    assert!(count <= u16::MAX as usize, "slice too large");

    let mut fl = 0u8;
    if info.keyframe {
        fl |= flags::KEYFRAME;
    }
    if info.slice_idx as u16 + 1 == info.slice_count as u16 {
        fl |= flags::LAST_SLICE;
    }

    (0..count)
        .map(|i| {
            let start = i * max_payload;
            let end = (start + max_payload).min(data.len());
            let chunk = &data[start..end];
            let h = VideoHeader {
                flags: fl,
                slice_idx: info.slice_idx,
                slice_count: info.slice_count,
                frame_id: info.frame_id,
                pose_timestamp_us: info.pose_timestamp_us,
                packet_idx: i as u16,
                packet_count: count as u16,
                payload_len: chunk.len() as u16,
            };
            let mut p = Vec::with_capacity(VIDEO_HEADER_LEN + chunk.len());
            h.write_to(&mut p);
            p.extend_from_slice(chunk);
            p
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info() -> SliceInfo {
        SliceInfo { frame_id: 42, slice_idx: 1, slice_count: 2, keyframe: true, pose_timestamp_us: 123_456 }
    }

    #[test]
    fn header_roundtrip() {
        let h = VideoHeader {
            flags: flags::KEYFRAME,
            slice_idx: 0,
            slice_count: 4,
            frame_id: 7,
            pose_timestamp_us: 99,
            packet_idx: 2,
            packet_count: 5,
            payload_len: 3,
        };
        let mut buf = Vec::new();
        h.write_to(&mut buf);
        assert_eq!(buf.len(), VIDEO_HEADER_LEN);
        buf.extend_from_slice(&[1, 2, 3]);
        let (parsed, payload) = VideoHeader::parse(&buf).unwrap();
        assert_eq!(parsed, h);
        assert_eq!(payload, &[1, 2, 3]);
    }

    #[test]
    fn packetize_splits_and_reassembles() {
        let data: Vec<u8> = (0..3000u32).map(|i| (i % 251) as u8).collect();
        let packets = packetize_slice(&info(), &data, 1200);
        assert_eq!(packets.len(), 3);
        let mut out = Vec::new();
        let mut lens = Vec::new();
        for (i, p) in packets.iter().enumerate() {
            let (h, payload) = VideoHeader::parse(p).unwrap();
            assert_eq!(h.packet_idx as usize, i);
            assert_eq!(h.packet_count, 3);
            assert_eq!(h.frame_id, 42);
            assert_ne!(h.flags & flags::KEYFRAME, 0);
            assert_ne!(h.flags & flags::LAST_SLICE, 0);
            lens.push(payload.len());
            out.extend_from_slice(payload);
        }
        assert_eq!(lens, vec![1200, 1200, 600]);
        assert_eq!(out, data);
    }

    #[test]
    fn empty_slice_is_one_packet() {
        let packets = packetize_slice(&info(), &[], 1200);
        assert_eq!(packets.len(), 1);
        let (h, payload) = VideoHeader::parse(&packets[0]).unwrap();
        assert_eq!(h.payload_len, 0);
        assert!(payload.is_empty());
    }

    #[test]
    fn rejects_bad_input() {
        assert_eq!(VideoHeader::parse(&[1, 0, 0]), Err(DecodeError::Truncated));
        let mut buf = vec![9u8; VIDEO_HEADER_LEN];
        assert_eq!(VideoHeader::parse(&buf), Err(DecodeError::BadType(9)));
        buf[0] = PACKET_TYPE_VIDEO;
        buf[20] = 10; // claims 10 payload bytes that are not there
        buf[21] = 0;
        assert_eq!(VideoHeader::parse(&buf), Err(DecodeError::Truncated));
    }
}
