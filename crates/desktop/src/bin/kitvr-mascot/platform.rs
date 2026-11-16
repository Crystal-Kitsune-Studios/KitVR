//! The bits of the desktop the mascot needs outside its own window: where the cursor is (even when
//! the window lets clicks through and gets no events), whether the left button is held, and the
//! work area (the screen minus panels and the taskbar). All values are physical pixels.

#[derive(Debug, Clone, Copy)]
pub struct Cursor {
    pub x: f32,
    pub y: f32,
    pub left_down: bool,
}

/// x, y, width, height.
pub type WorkArea = [f32; 4];

#[cfg(target_os = "linux")]
pub struct Desktop {
    xlib: x11_dl::xlib::Xlib,
    display: *mut x11_dl::xlib::Display,
    root: x11_dl::xlib::Window,
    workarea_atom: x11_dl::xlib::Atom,
}

#[cfg(target_os = "linux")]
impl Desktop {
    pub fn new() -> Option<Self> {
        let xlib = x11_dl::xlib::Xlib::open().ok()?;
        unsafe {
            let display = (xlib.XOpenDisplay)(std::ptr::null());
            if display.is_null() {
                return None;
            }
            let root = (xlib.XDefaultRootWindow)(display);
            let workarea_atom = (xlib.XInternAtom)(display, c"_NET_WORKAREA".as_ptr(), 0);
            Some(Self { xlib, display, root, workarea_atom })
        }
    }

    pub fn cursor(&self) -> Option<Cursor> {
        let (mut root, mut child) = (0, 0);
        let (mut x, mut y, mut wx, mut wy) = (0, 0, 0, 0);
        let mut mask = 0;
        let ok = unsafe {
            (self.xlib.XQueryPointer)(
                self.display, self.root, &mut root, &mut child, &mut x, &mut y, &mut wx, &mut wy, &mut mask,
            )
        };
        (ok != 0).then_some(Cursor { x: x as f32, y: y as f32, left_down: mask & x11_dl::xlib::Button1Mask != 0 })
    }

    pub fn work_area(&self) -> Option<WorkArea> {
        if self.workarea_atom == 0 {
            return None;
        }
        let mut actual_type = 0;
        let mut actual_format = 0;
        let mut items = 0;
        let mut remaining = 0;
        let mut data: *mut u8 = std::ptr::null_mut();
        unsafe {
            let status = (self.xlib.XGetWindowProperty)(
                self.display, self.root, self.workarea_atom, 0, 4, 0, x11_dl::xlib::XA_CARDINAL,
                &mut actual_type, &mut actual_format, &mut items, &mut remaining, &mut data,
            );
            if status != 0 || data.is_null() {
                return None;
            }
            // Format 32 properties come back as C longs.
            let values = std::slice::from_raw_parts(data as *const std::os::raw::c_long, items as usize);
            let area = (actual_format == 32 && values.len() >= 4)
                .then(|| [values[0] as f32, values[1] as f32, values[2] as f32, values[3] as f32]);
            (self.xlib.XFree)(data as *mut _);
            area.filter(|a| a[2] > 0.0 && a[3] > 0.0)
        }
    }
}

#[cfg(target_os = "linux")]
impl Drop for Desktop {
    fn drop(&mut self) {
        unsafe { (self.xlib.XCloseDisplay)(self.display) };
    }
}

#[cfg(windows)]
pub struct Desktop;

#[cfg(windows)]
impl Desktop {
    pub fn new() -> Option<Self> {
        Some(Self)
    }

    pub fn cursor(&self) -> Option<Cursor> {
        use windows_sys::Win32::Foundation::POINT;
        use windows_sys::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON};
        use windows_sys::Win32::UI::WindowsAndMessaging::GetCursorPos;
        let mut point = POINT { x: 0, y: 0 };
        if unsafe { GetCursorPos(&mut point) } == 0 {
            return None;
        }
        let left_down = unsafe { GetAsyncKeyState(VK_LBUTTON as i32) } < 0;
        Some(Cursor { x: point.x as f32, y: point.y as f32, left_down })
    }

    pub fn work_area(&self) -> Option<WorkArea> {
        use windows_sys::Win32::Foundation::RECT;
        use windows_sys::Win32::UI::WindowsAndMessaging::{SystemParametersInfoW, SPI_GETWORKAREA};
        let mut rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        let ok = unsafe { SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut rect as *mut RECT as *mut _, 0) };
        (ok != 0).then(|| {
            [rect.left as f32, rect.top as f32, (rect.right - rect.left) as f32, (rect.bottom - rect.top) as f32]
        })
    }
}

#[cfg(not(any(target_os = "linux", windows)))]
pub struct Desktop;

#[cfg(not(any(target_os = "linux", windows)))]
impl Desktop {
    pub fn new() -> Option<Self> {
        None
    }
    pub fn cursor(&self) -> Option<Cursor> {
        None
    }
    pub fn work_area(&self) -> Option<WorkArea> {
        None
    }
}
