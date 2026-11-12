//! Reader for the frame ring published by the KitVR Vulkan layer in `linux/vklayer`.
//!
//! SteamVR on Linux cannot hand frames to an OpenVR driver, so the layer copies every presented
//! swapchain image into a POSIX shared memory ring and the server streams it as rawvideo. The ring
//! layout is documented in `linux/README.md` and written by `linux/vklayer/kitvr_layer.cpp`.

use std::ffi::CString;
use std::io;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

/// Shared memory name, also overridable with `KITVR_STEAMVR_SHM` on both sides.
pub const DEFAULT_NAME: &str = "kitvr-steamvr";

const MAGIC: u32 = 0x4B56_5246; // "KVRF"
const VERSION: u32 = 1;
const HEADER_BYTES: usize = 48;
const FRAME_ID_OFFSET: usize = 32;

pub const FORMAT_BGRA8: u32 = 0;
pub const FORMAT_RGBA8: u32 = 1;

/// A mapped view of the shared memory ring. Frames are published by the layer with a release store
/// on the frame id, so a reader never observes a half written frame under a new id.
#[derive(Debug)]
pub struct SteamVrRing {
    base: *mut u8,
    len: usize,
    width: u32,
    height: u32,
    format: u32,
    last_frame: AtomicU64,
}

// The mapping is owned by this struct and is only ever read after construction.
unsafe impl Send for SteamVrRing {}
unsafe impl Sync for SteamVrRing {}

impl Drop for SteamVrRing {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.base as *mut libc::c_void, self.len);
        }
    }
}

fn read_u32(base: *const u8, offset: usize) -> u32 {
    unsafe { std::ptr::read_unaligned(base.add(offset) as *const u32) }
}

impl SteamVrRing {
    /// Maps the ring published by the layer. Fails when SteamVR is not running or the layer is not
    /// installed, which is the common case and worth a clear message.
    pub fn open(name: &str) -> io::Result<Arc<Self>> {
        // Overridable so a sandboxed server (for example a Flatpak) can be pointed at a directory
        // both sandboxes can see.
        let directory = std::env::var("KITVR_STEAMVR_SHM_DIR")
            .ok()
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| "/dev/shm".to_string());
        let full_path = format!("{directory}/{name}");
        let path = CString::new(full_path.clone())
            .map_err(|_| io::Error::other("invalid SteamVR shared memory name"))?;
        let fd = unsafe { libc::open(path.as_ptr(), libc::O_RDONLY) };
        if fd < 0 {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "no SteamVR frame ring at {full_path}. Start SteamVR with the KitVR layer \
                     installed, or set KITVR_STEAMVR_SHM and KITVR_STEAMVR_SHM_DIR to match the layer."
                ),
            ));
        }
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut stat) } != 0 {
            let error = io::Error::last_os_error();
            unsafe { libc::close(fd) };
            return Err(error);
        }
        let len = stat.st_size.max(0) as usize;
        if len < HEADER_BYTES {
            unsafe { libc::close(fd) };
            return Err(io::Error::other(
                "SteamVR frame ring is smaller than its header; the layer may not have started",
            ));
        }
        let mapped = unsafe {
            libc::mmap(std::ptr::null_mut(), len, libc::PROT_READ, libc::MAP_SHARED, fd, 0)
        };
        unsafe { libc::close(fd) };
        if mapped == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let base = mapped as *mut u8;

        let fail = |message: String| {
            unsafe {
                libc::munmap(mapped, len);
            }
            io::Error::other(message)
        };

        if read_u32(base, 0) != MAGIC {
            return Err(fail("SteamVR frame ring has the wrong magic number".to_string()));
        }
        let version = read_u32(base, 4);
        if version != VERSION {
            return Err(fail(format!(
                "SteamVR frame ring version {version} is not supported (expected {VERSION})"
            )));
        }
        let width = read_u32(base, 8);
        let height = read_u32(base, 12);
        let format = read_u32(base, 16);
        let stride = read_u32(base, 20);
        let payload = read_u32(base, 24);
        if width == 0 || height == 0 {
            return Err(fail("SteamVR frame ring has no frame geometry yet".to_string()));
        }
        if format != FORMAT_BGRA8 && format != FORMAT_RGBA8 {
            return Err(fail(format!("unsupported SteamVR frame format {format}")));
        }
        if stride != width * 4 {
            return Err(fail(format!(
                "SteamVR frame stride {stride} does not match {width} pixels wide"
            )));
        }
        if HEADER_BYTES + payload as usize > len {
            return Err(fail("SteamVR frame ring is smaller than its payload".to_string()));
        }

        Ok(Arc::new(Self {
            base,
            len,
            width,
            height,
            format,
            last_frame: AtomicU64::new(0),
        }))
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// ffmpeg pixel format name for the captured image.
    pub fn pixel_format(&self) -> &'static str {
        if self.format == FORMAT_RGBA8 {
            "rgba"
        } else {
            "bgra"
        }
    }

    /// Copies the newest frame into `out` when it is newer than the previous call.
    pub fn read_frame(&self, out: &mut Vec<u8>) -> bool {
        let id = unsafe {
            (*(self.base.add(FRAME_ID_OFFSET) as *const AtomicU64)).load(Ordering::Acquire)
        };
        if id == 0 || id == self.last_frame.load(Ordering::Relaxed) {
            return false;
        }
        let payload = read_u32(self.base, 24) as usize;
        let pixels = unsafe { std::slice::from_raw_parts(self.base.add(HEADER_BYTES), payload) };
        out.clear();
        out.extend_from_slice(pixels);
        self.last_frame.store(id, Ordering::Relaxed);
        true
    }
}
