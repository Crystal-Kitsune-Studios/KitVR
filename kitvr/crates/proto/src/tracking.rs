//! Tracking packets, client to server over UDP. 136 bytes, little-endian.
//!
//!   0   u8   type (2)
//!   1   u8   flags (0)
//!   2   u16  seq
//!   4   u64  timestamp_us (client monotonic clock)
//!   12  28B  head pose (position xyz, orientation xyzw, all f32)
//!   40  48B  left hand block
//!   88  48B  right hand block
//!
//! Hand block: u16 buttons, u16 flags (bit0 active), f32 trigger, f32 grip,
//! f32 stick_x, f32 stick_y, then a 28 byte pose.

use crate::DecodeError;

pub const PACKET_TYPE_TRACKING: u8 = 2;
pub const TRACKING_LEN: usize = 136;

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Pose {
    pub position: [f32; 3],
    pub orientation: [f32; 4],
}

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Hand {
    pub buttons: u16,
    pub active: bool,
    pub trigger: f32,
    pub grip: f32,
    pub stick: [f32; 2],
    pub pose: Pose,
}

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Tracking {
    pub seq: u16,
    pub timestamp_us: u64,
    pub head: Pose,
    pub left: Hand,
    pub right: Hand,
}

fn put_f32(out: &mut Vec<u8>, v: f32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn get_f32(b: &[u8]) -> f32 {
    f32::from_le_bytes(b[0..4].try_into().unwrap())
}

impl Pose {
    fn write_to(&self, out: &mut Vec<u8>) {
        for v in self.position.iter().chain(self.orientation.iter()) {
            put_f32(out, *v);
        }
    }

    fn parse(b: &[u8]) -> Pose {
        let f = |i: usize| get_f32(&b[i * 4..]);
        Pose { position: [f(0), f(1), f(2)], orientation: [f(3), f(4), f(5), f(6)] }
    }
}

impl Hand {
    fn write_to(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.buttons.to_le_bytes());
        out.extend_from_slice(&(self.active as u16).to_le_bytes());
        put_f32(out, self.trigger);
        put_f32(out, self.grip);
        put_f32(out, self.stick[0]);
        put_f32(out, self.stick[1]);
        self.pose.write_to(out);
    }

    fn parse(b: &[u8]) -> Hand {
        Hand {
            buttons: u16::from_le_bytes(b[0..2].try_into().unwrap()),
            active: u16::from_le_bytes(b[2..4].try_into().unwrap()) & 1 != 0,
            trigger: get_f32(&b[4..]),
            grip: get_f32(&b[8..]),
            stick: [get_f32(&b[12..]), get_f32(&b[16..])],
            pose: Pose::parse(&b[20..48]),
        }
    }
}

impl Tracking {
    pub fn write_to(&self, out: &mut Vec<u8>) {
        out.push(PACKET_TYPE_TRACKING);
        out.push(0);
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.timestamp_us.to_le_bytes());
        self.head.write_to(out);
        self.left.write_to(out);
        self.right.write_to(out);
    }

    pub fn parse(buf: &[u8]) -> Result<Self, DecodeError> {
        if buf.len() < TRACKING_LEN {
            return Err(DecodeError::Truncated);
        }
        if buf[0] != PACKET_TYPE_TRACKING {
            return Err(DecodeError::BadType(buf[0]));
        }
        Ok(Self {
            seq: u16::from_le_bytes(buf[2..4].try_into().unwrap()),
            timestamp_us: u64::from_le_bytes(buf[4..12].try_into().unwrap()),
            head: Pose::parse(&buf[12..40]),
            left: Hand::parse(&buf[40..88]),
            right: Hand::parse(&buf[88..136]),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_and_length() {
        let t = Tracking {
            seq: 513,
            timestamp_us: 9_876_543_210,
            head: Pose { position: [0.1, 1.6, -0.2], orientation: [0.0, 0.7, 0.0, 0.7] },
            left: Hand { buttons: 5, active: true, trigger: 0.5, grip: 1.0, stick: [-1.0, 0.25], pose: Pose::default() },
            right: Hand::default(),
        };
        let mut buf = Vec::new();
        t.write_to(&mut buf);
        assert_eq!(buf.len(), TRACKING_LEN);
        assert_eq!(Tracking::parse(&buf).unwrap(), t);
    }

    #[test]
    fn rejects_short_and_wrong_type() {
        assert_eq!(Tracking::parse(&[2u8; 10]), Err(DecodeError::Truncated));
        let mut buf = vec![0u8; TRACKING_LEN];
        buf[0] = 1;
        assert_eq!(Tracking::parse(&buf), Err(DecodeError::BadType(1)));
    }
}
