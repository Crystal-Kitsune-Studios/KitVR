use std::io;

use kitvr_core::proto::tracking::Tracking;

#[derive(Debug, Default, PartialEq)]
struct MouseUpdate {
    dx: f64,
    dy: f64,
    left: bool,
    right: bool,
}

#[derive(Default)]
struct Mapper {
    last_seq: Option<u16>,
    position: Option<[f32; 2]>,
}

impl Mapper {
    fn update(&mut self, tracking: &Tracking) -> Option<MouseUpdate> {
        if self.last_seq.is_some_and(|last| (tracking.seq.wrapping_sub(last) as i16) <= 0) {
            return None;
        }
        self.last_seq = Some(tracking.seq);
        let hand = &tracking.right;
        let [x, y, z] = hand.pose.position;
        if !hand.active || !x.is_finite() || !y.is_finite() || !z.is_finite() {
            self.position = None;
            return Some(MouseUpdate::default());
        }
        let mut update = MouseUpdate {
            left: hand.buttons & 1 != 0,
            right: hand.buttons & (1 << 1) != 0 && hand.buttons & 1 == 0,
            ..Default::default()
        };
        if let Some([old_x, old_y]) = self.position {
            let dx = x - old_x;
            let dy = y - old_y;
            let distance = dx.hypot(dy);
            if distance > 0.15 {
                self.position = Some([x, y]);
            } else if distance >= 0.002 {
                update.dx = f64::from(dx) * 2500.0;
                update.dy = -f64::from(dy) * 2500.0;
                self.position = Some([x, y]);
            }
        } else {
            self.position = Some([x, y]);
        }
        Some(update)
    }

    fn reset(&mut self) {
        *self = Self::default();
    }
}

pub struct Mouse {
    backend: Backend,
}

impl Mouse {
    pub fn new() -> io::Result<Self> {
        Ok(Self { backend: Backend::new()? })
    }

    pub fn session(&self) -> MouseSession<'_> {
        MouseSession { mouse: self, mapper: Mapper::default(), left: false, right: false }
    }
}

pub struct MouseSession<'a> {
    mouse: &'a Mouse,
    mapper: Mapper,
    left: bool,
    right: bool,
}

impl MouseSession<'_> {
    pub fn update(&mut self, tracking: &Tracking) -> io::Result<bool> {
        if let Some(update) = self.mapper.update(tracking) {
            self.apply(update)?;
            return Ok(true);
        }
        Ok(false)
    }

    pub fn reset(&mut self) -> io::Result<()> {
        self.mapper.reset();
        self.apply(MouseUpdate::default())
    }

    fn apply(&mut self, update: MouseUpdate) -> io::Result<()> {
        if update.dx != 0.0 || update.dy != 0.0 {
            self.mouse.backend.motion(update.dx, update.dy)?;
        }
        if update.left != self.left {
            self.mouse.backend.button(0, update.left)?;
            self.left = update.left;
        }
        if update.right != self.right {
            self.mouse.backend.button(1, update.right)?;
            self.right = update.right;
        }
        Ok(())
    }
}

impl Drop for MouseSession<'_> {
    fn drop(&mut self) {
        if let Err(error) = self.reset() {
            eprintln!("could not release mouse buttons: {error}");
        }
    }
}

#[cfg(target_os = "linux")]
struct Backend {
    runtime: tokio::runtime::Runtime,
    portal: ashpd::desktop::remote_desktop::RemoteDesktop,
    session: ashpd::desktop::Session<ashpd::desktop::remote_desktop::RemoteDesktop>,
}

#[cfg(target_os = "linux")]
impl Backend {
    fn new() -> io::Result<Self> {
        use ashpd::desktop::remote_desktop::{DeviceType, RemoteDesktop, SelectDevicesOptions};
        let runtime = tokio::runtime::Runtime::new()?;
        println!("Approve mouse control in the desktop remote-control dialog.");
        let (portal, session) = runtime.block_on(async {
            let portal = RemoteDesktop::new().await.map_err(portal_error)?;
            let session = portal.create_session(Default::default()).await.map_err(portal_error)?;
            portal.select_devices(&session, SelectDevicesOptions::default().set_devices(DeviceType::POINTER))
                .await.map_err(portal_error)?.response().map_err(portal_error)?;
            let response = portal.start(&session, None, Default::default()).await
                .map_err(portal_error)?.response().map_err(portal_error)?;
            if !response.devices().contains(DeviceType::POINTER) {
                return Err(io::Error::other("mouse control was not granted by the portal"));
            }
            Ok::<_, io::Error>((portal, session))
        })?;
        Ok(Self { runtime, portal, session })
    }

    fn motion(&self, dx: f64, dy: f64) -> io::Result<()> {
        self.runtime.block_on(self.portal.notify_pointer_motion(&self.session, dx, dy, Default::default()))
            .map_err(portal_error)
    }

    fn button(&self, button: u8, down: bool) -> io::Result<()> {
        use ashpd::desktop::remote_desktop::KeyState;
        let state = if down { KeyState::Pressed } else { KeyState::Released };
        let code = if button == 0 { 0x110 } else { 0x111 };
        self.runtime.block_on(self.portal.notify_pointer_button(&self.session, code, state, Default::default()))
            .map_err(portal_error)
    }
}

#[cfg(target_os = "linux")]
fn portal_error(error: ashpd::Error) -> io::Error {
    io::Error::other(format!("desktop mouse control failed: {error}"))
}

#[cfg(windows)]
struct Backend;

#[cfg(windows)]
impl Backend {
    fn new() -> io::Result<Self> { Ok(Self) }

    fn motion(&self, dx: f64, dy: f64) -> io::Result<()> {
        use windows_sys::Win32::UI::Input::KeyboardAndMouse::MOUSEEVENTF_MOVE;
        send_mouse(dx.round() as i32, dy.round() as i32, MOUSEEVENTF_MOVE)
    }

    fn button(&self, button: u8, down: bool) -> io::Result<()> {
        use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
            MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP,
        };
        let flags = match (button, down) {
            (0, true) => MOUSEEVENTF_LEFTDOWN,
            (0, false) => MOUSEEVENTF_LEFTUP,
            (_, true) => MOUSEEVENTF_RIGHTDOWN,
            (_, false) => MOUSEEVENTF_RIGHTUP,
        };
        send_mouse(0, 0, flags)
    }
}

#[cfg(windows)]
fn send_mouse(dx: i32, dy: i32, flags: u32) -> io::Result<()> {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{SendInput, INPUT, INPUT_0, INPUT_MOUSE, MOUSEINPUT};
    let input = INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 { mi: MOUSEINPUT { dx, dy, mouseData: 0, dwFlags: flags, time: 0, dwExtraInfo: 0 } },
    };
    if unsafe { SendInput(1, &input, std::mem::size_of::<INPUT>() as i32) } != 1 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use kitvr_core::proto::tracking::Hand;

    #[test]
    fn tracks_movement_clicks_and_rejects_stale_packets() {
        let mut mapper = Mapper::default();
        let mut packet = Tracking { seq: 65535, right: Hand { active: true, ..Default::default() }, ..Default::default() };
        assert_eq!(mapper.update(&packet), Some(MouseUpdate::default()));
        packet.seq = 0;
        packet.right.pose.position = [0.01, 0.02, 0.0];
        packet.right.buttons = 1;
        let update = mapper.update(&packet).unwrap();
        assert!((update.dx - 25.0).abs() < 0.001);
        assert!((update.dy + 50.0).abs() < 0.001);
        assert!(update.left && !update.right);
        assert_eq!(mapper.update(&packet), None);
        packet.seq = 1;
        packet.right.active = false;
        assert_eq!(mapper.update(&packet), Some(MouseUpdate::default()));
    }

    #[test]
    fn invalid_and_large_movements_do_not_move_pointer() {
        let mut mapper = Mapper::default();
        let mut packet = Tracking { right: Hand { active: true, ..Default::default() }, ..Default::default() };
        mapper.update(&packet);
        packet.seq += 1;
        packet.right.pose.position[0] = 100.0;
        assert_eq!(mapper.update(&packet).unwrap().dx, 0.0);
        packet.seq += 1;
        packet.right.pose.position[0] = f32::NAN;
        packet.right.buttons = 1;
        assert_eq!(mapper.update(&packet), Some(MouseUpdate::default()));
    }

    #[test]
    fn right_click_and_reset_after_tracking_loss() {
        let mut mapper = Mapper::default();
        let mut packet = Tracking {
            seq: 10,
            right: Hand { active: true, buttons: 2, ..Default::default() },
            ..Default::default()
        };
        assert!(mapper.update(&packet).unwrap().right);
        mapper.reset();
        packet.seq = 1;
        packet.right.pose.position[0] = 2.0;
        packet.right.buttons = 0;
        assert_eq!(mapper.update(&packet), Some(MouseUpdate::default()));
    }
}
