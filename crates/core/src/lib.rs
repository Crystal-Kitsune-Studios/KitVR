//! Platform-neutral interfaces. Each OS backend in the server crate implements these.

use std::any::Any;
use std::fmt;

pub use kitvr_proto as proto;

#[derive(Debug)]
pub struct BackendError(pub String);

impl BackendError {
    pub fn unimplemented(what: &str) -> Self {
        Self(format!("{what}: not implemented yet"))
    }
}

impl fmt::Display for BackendError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::error::Error for BackendError {}

pub type Result<T> = std::result::Result<T, BackendError>;

/// Captured frame. Keep it on the GPU whenever possible.
pub enum FrameData {
    /// Slow path, useful for bring-up and tests.
    Cpu { stride: usize, bgra: Vec<u8> },
    /// Backend-specific zero-copy handle (D3D11 texture on Windows, DMA-BUF on Linux).
    /// The encoder from the same backend knows how to downcast it.
    Native(Box<dyn Any + Send>),
}

pub struct Frame {
    pub width: u32,
    pub height: u32,
    pub timestamp_us: u64,
    pub data: FrameData,
}

pub trait CaptureSource: Send {
    /// Blocks until the next frame is available.
    fn next_frame(&mut self) -> Result<Frame>;
}

pub struct EncodedSlice {
    pub frame_id: u32,
    pub slice_idx: u8,
    pub slice_count: u8,
    pub keyframe: bool,
    pub pose_timestamp_us: u64,
    pub data: Vec<u8>,
}

pub trait VideoEncoder: Send {
    fn configure(&mut self, cfg: &proto::control::Message) -> Result<()>;
    /// Calls `out` for each slice as soon as it is ready, not after the whole frame.
    fn encode(&mut self, frame: Frame, out: &mut dyn FnMut(EncodedSlice)) -> Result<()>;
    fn request_keyframe(&mut self);
    fn set_bitrate_kbps(&mut self, kbps: u32);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
    Middle,
}

pub trait InputInjector: Send {
    /// Coordinates are normalized 0.0..=1.0 over the captured display.
    fn mouse_move_abs(&mut self, x: f32, y: f32) -> Result<()>;
    fn mouse_button(&mut self, button: MouseButton, down: bool) -> Result<()>;
    fn scroll(&mut self, dx: f32, dy: f32) -> Result<()>;
    /// `code` is a USB HID usage code, so both OS backends translate from one table.
    fn key(&mut self, code: u32, down: bool) -> Result<()>;
}
