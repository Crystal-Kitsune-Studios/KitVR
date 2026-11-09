//! Wire protocol shared by the PC server and (via a C ABI or a port) the Quest client.
//! See docs/PROTOCOL.md for the normative description. No dependencies on purpose.

pub mod audio;
pub mod control;
pub mod tracking;
pub mod video;

pub const PROTOCOL_VERSION: u16 = 1;
pub const CONTROL_PORT: u16 = 9943; // TCP
pub const MEDIA_PORT: u16 = 9944; // UDP
/// Payload bytes per UDP packet, excluding our header. Keeps packets under a 1500 MTU.
pub const MAX_PACKET_PAYLOAD: usize = 1200;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeError {
    /// Not enough bytes yet. For TCP, read more and retry.
    Truncated,
    BadType(u8),
    BadLength,
    BadUtf8,
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DecodeError::Truncated => write!(f, "truncated message"),
            DecodeError::BadType(t) => write!(f, "unknown type {t}"),
            DecodeError::BadLength => write!(f, "bad length"),
            DecodeError::BadUtf8 => write!(f, "invalid utf-8"),
        }
    }
}

impl std::error::Error for DecodeError {}
