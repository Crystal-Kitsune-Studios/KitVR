use kitvr_core::{BackendError, CaptureSource, InputInjector, Result, VideoEncoder};

pub const NAME: &str = "windows";

pub fn create_capture() -> Result<Box<dyn CaptureSource>> {
    // TODO: Windows.Graphics.Capture, keep frames as D3D11 textures.
    Err(BackendError::unimplemented("Windows.Graphics.Capture"))
}

pub fn create_encoder() -> Result<Box<dyn VideoEncoder>> {
    // TODO: FFmpeg (hevc_nvenc / hevc_amf / hevc_qsv) with D3D11 hw frames.
    Err(BackendError::unimplemented("NVENC/AMF/QSV encoder"))
}

pub fn create_input() -> Result<Box<dyn InputInjector>> {
    // TODO: SendInput.
    Err(BackendError::unimplemented("SendInput injector"))
}
