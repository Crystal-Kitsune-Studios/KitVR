use kitvr_core::{BackendError, CaptureSource, InputInjector, Result, VideoEncoder};

pub const NAME: &str = "linux";

pub fn create_capture() -> Result<Box<dyn CaptureSource>> {
    // TODO: PipeWire via xdg-desktop-portal ScreenCast (Wayland), X11 fallback.
    // Request DMA-BUF buffers so frames stay on the GPU.
    Err(BackendError::unimplemented("PipeWire ScreenCast"))
}

pub fn create_encoder() -> Result<Box<dyn VideoEncoder>> {
    // TODO: FFmpeg (hevc_nvenc / hevc_vaapi) with DMA-BUF import.
    Err(BackendError::unimplemented("NVENC/VAAPI encoder"))
}

pub fn create_input() -> Result<Box<dyn InputInjector>> {
    // TODO: /dev/uinput virtual device, plus a udev rule in packaging/.
    Err(BackendError::unimplemented("uinput injector"))
}
