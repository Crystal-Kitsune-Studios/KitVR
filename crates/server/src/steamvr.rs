//! Reader for the SteamVR frame ring.
//!
//! On Linux SteamVR cannot hand frames to an OpenVR driver, so the Vulkan layer in `linux/vklayer`
//! copies every presented swapchain image into a POSIX shared memory ring. On Windows the KitVR
//! driver receives the compositor's backbuffer through `IVRVirtualDisplay` and publishes it into a
//! named section (`Local\\<name>`) with the same layout. Either way the server streams it as
//! rawvideo. The layout is documented in `linux/README.md`.

use std::io;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

/// Shared memory name, also overridable with `KITVR_STEAMVR_SHM` on both sides.
pub const DEFAULT_NAME: &str = "kitvr-steamvr";

const MAGIC: u32 = 0x4B56_5246; // "KVRF"
const VERSION: u32 = 1;
const HEADER_BYTES: usize = 48;
const FRAME_ID_OFFSET: usize = 32;

pub const FORMAT_BGRA8: u32 = 0;
pub const FORMAT_RGBA8: u32 = 1;
pub const FORMAT_X2RGB10: u32 = 2;
pub const FORMAT_X2BGR10: u32 = 3;

/// A mapped view of the shared memory ring. Frames are published with a release store on the
/// frame id, so a reader never observes a half written frame under a new id.
#[derive(Debug)]
pub struct SteamVrRing {
    mapping: Mapping,
    width: u32,
    height: u32,
    format: u32,
    last_frame: AtomicU64,
    geometry_warned: AtomicBool,
}

// The mapping is owned by this struct and is only ever read after construction.
unsafe impl Send for SteamVrRing {}
unsafe impl Sync for SteamVrRing {}

/// A read-only mapping of the ring. `len` is the number of readable bytes from `base`.
#[derive(Debug)]
struct Mapping {
    base: *mut u8,
    len: usize,
    #[cfg(windows)]
    handle: windows_sys::Win32::Foundation::HANDLE,
}

#[cfg(unix)]
impl Mapping {
    fn open(name: &str) -> io::Result<Self> {
        use std::ffi::CString;
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
        Ok(Self { base: mapped as *mut u8, len })
    }
}

#[cfg(unix)]
impl Drop for Mapping {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.base as *mut libc::c_void, self.len);
        }
    }
}

#[cfg(windows)]
impl Mapping {
    fn open(name: &str) -> io::Result<Self> {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Memory::{
            MapViewOfFile, OpenFileMappingW, UnmapViewOfFile, VirtualQuery, FILE_MAP_READ,
            MEMORY_BASIC_INFORMATION, MEMORY_MAPPED_VIEW_ADDRESS,
        };
        let full_name = if name.contains('\\') { name.to_string() } else { format!("Local\\{name}") };
        let wide: Vec<u16> = full_name.encode_utf16().chain(std::iter::once(0)).collect();
        let handle = unsafe { OpenFileMappingW(FILE_MAP_READ, 0, wide.as_ptr()) };
        if handle.is_null() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "no SteamVR frame ring named {full_name}. Register the KitVR driver with \
                     vrpathreg and start SteamVR, or set KITVR_STEAMVR_SHM to match the driver."
                ),
            ));
        }
        let view = unsafe { MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0) };
        if view.Value.is_null() {
            let error = io::Error::last_os_error();
            unsafe { CloseHandle(handle) };
            return Err(error);
        }
        // The driver reserves a large section and commits pages as frames grow, so the readable
        // size is the committed region at the start of the view.
        let mut info: MEMORY_BASIC_INFORMATION = unsafe { std::mem::zeroed() };
        let queried = unsafe {
            VirtualQuery(view.Value, &mut info, std::mem::size_of::<MEMORY_BASIC_INFORMATION>())
        };
        const MEM_COMMIT: u32 = 0x1000;
        if queried == 0 || info.State != MEM_COMMIT {
            unsafe {
                UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: view.Value });
                CloseHandle(handle);
            }
            return Err(io::Error::other(
                "SteamVR frame ring is not ready yet; the driver has not published a frame",
            ));
        }
        Ok(Self { base: view.Value as *mut u8, len: info.RegionSize, handle })
    }
}

#[cfg(windows)]
impl Drop for Mapping {
    fn drop(&mut self) {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Memory::{UnmapViewOfFile, MEMORY_MAPPED_VIEW_ADDRESS};
        unsafe {
            UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.base as *mut core::ffi::c_void });
            CloseHandle(self.handle);
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
        let mapping = Mapping::open(name)?;
        let base = mapping.base;
        let len = mapping.len;
        if len < HEADER_BYTES {
            return Err(io::Error::other(
                "SteamVR frame ring is smaller than its header; the producer may not have started",
            ));
        }
        // Dropping `mapping` on an early return unmaps it.
        let fail = io::Error::other;

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
        if ![FORMAT_BGRA8, FORMAT_RGBA8, FORMAT_X2RGB10, FORMAT_X2BGR10].contains(&format) {
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
            mapping,
            width,
            height,
            format,
            last_frame: AtomicU64::new(0),
            geometry_warned: AtomicBool::new(false),
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
        match self.format {
            FORMAT_RGBA8 => "rgba",
            FORMAT_X2RGB10 => "x2rgb10le",
            FORMAT_X2BGR10 => "x2bgr10le",
            _ => "bgra",
        }
    }

    /// Copies the newest frame into `out` when it is newer than the previous call.
    pub fn read_frame(&self, out: &mut Vec<u8>) -> bool {
        let id = unsafe {
            (*(self.mapping.base.add(FRAME_ID_OFFSET) as *const AtomicU64)).load(Ordering::Acquire)
        };
        if id == 0 || id == self.last_frame.load(Ordering::Relaxed) {
            return false;
        }
        self.last_frame.store(id, Ordering::Relaxed);
        // ffmpeg was started for the geometry seen at open, and the mapping has the size seen at
        // open, so frames published after the compositor resized its window are skipped.
        let payload = read_u32(self.mapping.base, 24) as usize;
        if read_u32(self.mapping.base, 8) != self.width
            || read_u32(self.mapping.base, 12) != self.height
            || read_u32(self.mapping.base, 16) != self.format
            || HEADER_BYTES + payload > self.mapping.len
        {
            if !self.geometry_warned.swap(true, Ordering::Relaxed) {
                eprintln!(
                    "SteamVR frame size changed from {}x{}; restart the server to stream the new size",
                    self.width, self.height
                );
            }
            return false;
        }
        let pixels = unsafe { std::slice::from_raw_parts(self.mapping.base.add(HEADER_BYTES), payload) };
        out.clear();
        out.extend_from_slice(pixels);
        true
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Memory::{
        CreateFileMappingW, MapViewOfFile, UnmapViewOfFile, VirtualAlloc, FILE_MAP_WRITE,
        MEM_COMMIT, PAGE_READWRITE, SEC_RESERVE,
    };

    // Mirrors the driver's FrameRing: a reserved section with only the frame's pages committed.
    #[test]
    fn reads_frames_from_a_driver_style_section() {
        let name = "kitvr-steamvr-reader-test";
        let wide: Vec<u16> = format!("Local\\{name}").encode_utf16().chain(std::iter::once(0)).collect();
        let reserve: u32 = 64 << 20;
        let section = unsafe {
            CreateFileMappingW(INVALID_HANDLE_VALUE, std::ptr::null(), PAGE_READWRITE | SEC_RESERVE, 0, reserve, wide.as_ptr())
        };
        assert!(!section.is_null());
        let view = unsafe { MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0) };
        assert!(!view.Value.is_null());
        let base = view.Value as *mut u8;
        let (width, height) = (4u32, 2u32);
        let payload = width * height * 4;
        let used = HEADER_BYTES + payload as usize;
        assert!(!unsafe { VirtualAlloc(view.Value, used, MEM_COMMIT, PAGE_READWRITE) }.is_null());
        let header = [MAGIC, VERSION, width, height, FORMAT_RGBA8, width * 4, payload, 0];
        for (i, value) in header.iter().enumerate() {
            unsafe { std::ptr::write_unaligned(base.add(i * 4) as *mut u32, *value) };
        }
        for i in 0..payload as usize {
            unsafe { *base.add(HEADER_BYTES + i) = i as u8 };
        }

        let ring = SteamVrRing::open(name).expect("open ring");
        assert_eq!((ring.width(), ring.height(), ring.pixel_format()), (4, 2, "rgba"));
        let mut frame = Vec::new();
        assert!(!ring.read_frame(&mut frame), "frame id 0 means nothing published yet");
        unsafe { (*(base.add(FRAME_ID_OFFSET) as *const AtomicU64)).store(1, Ordering::Release) };
        assert!(ring.read_frame(&mut frame));
        assert_eq!(frame, (0..payload as u8).collect::<Vec<_>>());
        assert!(!ring.read_frame(&mut frame), "the same frame is not returned twice");

        drop(ring);
        unsafe {
            UnmapViewOfFile(view);
            CloseHandle(section);
        }
    }

    #[test]
    fn missing_section_is_not_found() {
        let error = SteamVrRing::open("kitvr-steamvr-does-not-exist").unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
    }
}
