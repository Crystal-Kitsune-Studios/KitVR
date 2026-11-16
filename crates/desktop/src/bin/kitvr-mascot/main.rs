//! KitVR desktop mascot: your VRM avatar lives on the desktop.
//!
//! It stands on the taskbar or panel, wanders along it, sits down for a rest, follows the cursor
//! with its head and blinks. Drag it to pick it up (it dangles and kicks), let go and it falls back
//! down; click it and it hops. Right-click for the menu. The window is click-through everywhere
//! except on the avatar itself.
//!
//! Usage: `kitvr-mascot [avatar.vrm]`, or drop a `.vrm` file onto it. The last avatar is
//! remembered.

#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod platform;
mod pose;
mod render;
mod vrm;

use std::collections::HashMap;
use std::f32::consts::TAU;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use eframe::egui::{self, pos2, vec2, Pos2, Vec2};
use eframe::egui_glow;
use glam::{Mat4, Vec3};

use pose::{Motion, Skeleton};
use vrm::Expression;

/// Window size in points. The avatar is framed to fill its height.
const SIZE: Vec2 = vec2(240.0, 400.0);
const GRAVITY: f32 = 2200.0;
const WALK_SPEED: f32 = 55.0;
const STORAGE_VRM: &str = "vrm_path";
const STORAGE_WANDER: &str = "wander";

#[derive(Debug, Clone, Copy, PartialEq)]
enum State {
    Idle { until: f32 },
    Walk { target_x: f32 },
    Sit { until: f32 },
    Falling,
    /// Left button held on the avatar; becomes a drag once the cursor moves.
    Pressed { origin: Pos2, grab: Vec2 },
    Dragged { grab: Vec2 },
}

struct Avatar {
    model: Arc<vrm::Model>,
    skeleton: Skeleton,
    /// Orthographic camera framing, from the rest pose.
    projection: Mat4,
    /// Head height in the window, as a fraction from the top, for aiming the look-at.
    head_fraction: f32,
}

struct MascotApp {
    scene: Arc<Mutex<render::Scene>>,
    desktop: Option<platform::Desktop>,
    avatar: Option<Avatar>,
    vrm_path: Option<PathBuf>,
    message: Option<String>,
    wander: bool,

    state: State,
    /// Window top-left in points; `None` until the first frame tells us where we are.
    position: Option<Pos2>,
    sent_position: Option<Pos2>,
    velocity: Vec2,
    passthrough: bool,
    menu: Option<Pos2>,

    start: Instant,
    last_frame: Instant,
    rng: u64,
    motion: Motion,
    look_target: (f32, f32),
    next_blink: f32,
    blink_start: f32,
    happy_until: f32,
}

impl MascotApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let stored = |key: &str| cc.storage.and_then(|storage| storage.get_string(key));
        let from_args = std::env::args_os().nth(1).map(PathBuf::from);
        let vrm_path = from_args.or_else(|| stored(STORAGE_VRM).map(PathBuf::from));
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0x9E37_79B9, |d| d.as_nanos() as u64)
            | 1;
        let mut app = Self {
            scene: Arc::new(Mutex::new(render::Scene::default())),
            desktop: platform::Desktop::new(),
            avatar: None,
            vrm_path: None,
            message: None,
            wander: stored(STORAGE_WANDER).map_or(true, |v| v != "false"),
            state: State::Falling,
            position: None,
            sent_position: None,
            velocity: Vec2::ZERO,
            passthrough: false,
            menu: None,
            start: Instant::now(),
            last_frame: Instant::now(),
            rng: seed,
            motion: Motion::default(),
            look_target: (0.0, 0.0),
            next_blink: 2.0,
            blink_start: -1.0,
            happy_until: 0.0,
        };
        if let Some(path) = vrm_path {
            app.load(&path);
        }
        app
    }

    fn random(&mut self) -> f32 {
        // xorshift64*
        self.rng ^= self.rng >> 12;
        self.rng ^= self.rng << 25;
        self.rng ^= self.rng >> 27;
        (self.rng.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 40) as f32 / (1u64 << 24) as f32
    }

    fn load(&mut self, path: &Path) {
        let result = std::fs::read(path)
            .map_err(|error| format!("could not read {}: {error}", path.display()))
            .and_then(|bytes| vrm::load(&bytes));
        match result {
            Ok(model) => {
                let skeleton = Skeleton::new(&model);
                let rest = skeleton.pose(&model, &pose::build(&Motion::default()));
                let (min, max) = Skeleton::bounds(&model, &rest);
                let height = (max.y - min.y).max(0.1);
                // Room above the head for raised arms; the feet sit on the bottom edge.
                let bottom = min.y - 0.01 * height;
                let top = max.y + 0.22 * height;
                let half_width = (top - bottom) * SIZE.x / SIZE.y / 2.0;
                let hips_x = model.meta.bones.get("hips").map_or(0.0, |&i| rest[i].w_axis.x);
                let projection = Mat4::orthographic_rh_gl(-half_width, half_width, bottom, top, -10.0, 10.0)
                    * Mat4::from_translation(Vec3::new(-hips_x, 0.0, 0.0));
                let head_y = model.meta.bones.get("head").map_or(max.y, |&i| rest[i].w_axis.y);
                let head_fraction = ((top - head_y) / (top - bottom)).clamp(0.0, 1.0);
                self.motion.sit_drop = skeleton.sit_drop;
                let model = Arc::new(model);
                {
                    let mut scene = self.scene.lock().unwrap();
                    scene.model = Some(Arc::clone(&model));
                    scene.generation += 1;
                    scene.error = None;
                }
                self.avatar = Some(Avatar { model, skeleton, projection, head_fraction });
                self.vrm_path = Some(path.to_path_buf());
                self.message = None;
                self.state = State::Falling;
            }
            Err(error) => {
                eprintln!("kitvr-mascot: {error}");
                self.message = Some(error);
            }
        }
    }

    fn work_area(&self, ctx: &egui::Context) -> egui::Rect {
        let ppp = ctx.pixels_per_point();
        if let Some([x, y, w, h]) = self.desktop.as_ref().and_then(|d| d.work_area()) {
            return egui::Rect::from_min_size(pos2(x / ppp, y / ppp), vec2(w / ppp, h / ppp));
        }
        let monitor = ctx.input(|i| i.viewport().monitor_size).unwrap_or(vec2(1920.0, 1080.0));
        egui::Rect::from_min_size(Pos2::ZERO, monitor)
    }

    fn step(&mut self, ctx: &egui::Context, now: f32, dt: f32, cursor: Option<(Pos2, bool)>, hit: bool) {
        let work = self.work_area(ctx);
        let ground = work.max.y - SIZE.y;
        let min_x = work.min.x - SIZE.x * 0.25;
        let max_x = work.max.x - SIZE.x * 0.75;
        let mut position = self.position.unwrap_or(pos2(max_x - 60.0, ground));

        let (pressed, secondary) = ctx.input(|i| (i.pointer.primary_pressed(), i.pointer.secondary_pressed()));
        let local_pointer = ctx.input(|i| i.pointer.interact_pos());
        if self.menu.is_none() && hit && !matches!(self.state, State::Pressed { .. } | State::Dragged { .. }) {
            if pressed {
                if let Some((cursor, _)) = cursor {
                    self.state = State::Pressed { origin: cursor, grab: cursor - position };
                }
            } else if secondary {
                self.menu = local_pointer;
            }
        }

        let button_down = cursor.map(|(_, down)| down).unwrap_or_else(|| ctx.input(|i| i.pointer.primary_down()));
        match self.state {
            State::Pressed { origin, grab } => {
                if let Some((cursor, _)) = cursor {
                    if (cursor - origin).length() > 6.0 {
                        self.state = State::Dragged { grab };
                    }
                }
                if !button_down && matches!(self.state, State::Pressed { .. }) {
                    // A click: hop with joy.
                    self.happy_until = now + 1.6;
                    self.velocity = vec2(0.0, -620.0);
                    self.state = State::Falling;
                }
            }
            State::Dragged { grab } => {
                if let Some((cursor, _)) = cursor {
                    let target = cursor - grab;
                    if dt > 0.0 {
                        let velocity = (target - position) / dt;
                        self.velocity = self.velocity * 0.7 + velocity * 0.3;
                    }
                    position = target;
                }
                if !button_down {
                    self.velocity = vec2(self.velocity.x.clamp(-900.0, 900.0), self.velocity.y.clamp(-900.0, 600.0));
                    self.state = State::Falling;
                }
            }
            State::Falling => {
                self.velocity.y += GRAVITY * dt;
                self.velocity.x *= (1.0 - 1.5 * dt).max(0.0);
                position += self.velocity * dt;
                if position.x < min_x || position.x > max_x {
                    position.x = position.x.clamp(min_x, max_x);
                    self.velocity.x = -self.velocity.x * 0.4;
                }
                if position.y >= ground {
                    position.y = ground;
                    self.velocity = Vec2::ZERO;
                    let rest = 1.0 + 2.0 * self.random();
                    self.state = State::Idle { until: now + rest };
                }
            }
            State::Idle { until } => {
                if now >= until {
                    let roll = self.random();
                    self.state = if self.wander && roll < 0.55 {
                        let span = (max_x - min_x).max(1.0);
                        State::Walk { target_x: min_x + span * self.random() }
                    } else if roll < 0.8 {
                        let length = 5.0 + 8.0 * self.random();
                        State::Sit { until: now + length }
                    } else {
                        let length = 2.0 + 3.0 * self.random();
                        State::Idle { until: now + length }
                    };
                }
            }
            State::Walk { target_x } => {
                let delta = target_x - position.x;
                // Wait for the body to turn before setting off.
                if self.motion.body_yaw.abs() > 0.8 {
                    let step = WALK_SPEED * dt * delta.signum();
                    if delta.abs() <= step.abs() || !self.wander {
                        position.x = if self.wander { target_x } else { position.x };
                        let rest = 1.5 + 3.0 * self.random();
                        self.state = State::Idle { until: now + rest };
                    } else {
                        position.x += step;
                    }
                }
            }
            State::Sit { until } => {
                if now >= until {
                    let rest = 1.0 + 2.0 * self.random();
                    self.state = State::Idle { until: now + rest };
                }
            }
        }
        let grounded = matches!(self.state, State::Idle { .. } | State::Walk { .. } | State::Sit { .. });
        if grounded && (position.y < ground - 1.0 || position.y > ground + 1.0) {
            // The panel moved or the window was placed elsewhere: fall (or snap) back down.
            if position.y > ground {
                position.y = ground;
            } else {
                self.state = State::Falling;
            }
        }
        if grounded {
            position.x = position.x.clamp(min_x, max_x);
        }
        self.position = Some(position);

        // Smooth the animation inputs toward the state's targets.
        let approach = |value: &mut f32, target: f32, rate: f32| {
            *value += (target - *value) * (1.0 - (-rate * dt).exp());
        };
        let walking = match self.state {
            State::Walk { target_x } => Some((target_x - position.x).signum()),
            _ => None,
        };
        let dragged = matches!(self.state, State::Dragged { .. });
        let sitting = matches!(self.state, State::Sit { .. });
        let m = &mut self.motion;
        m.time = now;
        approach(&mut m.walk, if walking.is_some() && m.body_yaw.abs() > 0.8 { 1.0 } else { 0.0 }, 6.0);
        approach(&mut m.body_yaw, walking.map_or(0.0, |dir| dir * 70f32.to_radians()), 5.0);
        approach(&mut m.sit, if sitting { 1.0 } else { 0.0 }, 3.0);
        approach(&mut m.dangle, if dragged { 1.0 } else { 0.0 }, 8.0);
        approach(&mut m.swing, if dragged { (-self.velocity.x / 1500.0).clamp(-0.5, 0.5) } else { 0.0 }, 6.0);
        approach(&mut m.cheer, if now < self.happy_until { 1.0 } else { 0.0 }, 8.0);
        if m.walk > 0.01 {
            m.walk_phase = (m.walk_phase + dt * TAU * 1.7) % TAU;
        }

        // Look at the cursor when it is nearby, ahead when walking.
        if let (Some(avatar), Some((cursor, _))) = (&self.avatar, cursor) {
            let head = position + vec2(SIZE.x / 2.0, SIZE.y * avatar.head_fraction);
            let offset = cursor - head;
            self.look_target = if offset.length() < 900.0 && walking.is_none() {
                (
                    (offset.x / 350.0).clamp(-1.0, 1.0) * 55f32.to_radians(),
                    (offset.y / 350.0).clamp(-1.0, 1.0) * 28f32.to_radians(),
                )
            } else {
                (0.0, 0.0)
            };
        }
        let (yaw, pitch) = self.look_target;
        // The head turn is relative to the body, which may already be turned.
        approach(&mut self.motion.look_yaw, (yaw - self.motion.body_yaw).clamp(-1.0, 1.0), 6.0);
        approach(&mut self.motion.look_pitch, pitch, 6.0);

        if now >= self.next_blink {
            self.blink_start = now;
            let gap = 2.0 + 4.0 * self.random();
            self.next_blink = now + gap;
        }
    }

    fn expressions(&self, now: f32) -> HashMap<Expression, f32> {
        let mut expressions = HashMap::new();
        let since = now - self.blink_start;
        let blink = if (0.0..0.16).contains(&since) { 1.0 - (since / 0.08 - 1.0).abs() } else { 0.0 };
        let happy = self.motion.cheer;
        let surprised = self.motion.dangle * (1.0 - happy);
        // Happy expressions usually close the eyes already.
        expressions.insert(Expression::Blink, blink * (1.0 - happy));
        expressions.insert(Expression::Happy, happy);
        expressions.insert(Expression::Surprised, surprised * 0.8);
        expressions
    }

    fn menu_ui(&mut self, ctx: &egui::Context) {
        let Some(at) = self.menu else { return };
        let mut close = false;
        let response = egui::Area::new(egui::Id::new("mascot-menu"))
            .fixed_pos(pos2(at.x.min(SIZE.x - 170.0).max(0.0), at.y.min(SIZE.y - 150.0).max(0.0)))
            .order(egui::Order::Foreground)
            .show(ctx, |ui| {
                egui::Frame::popup(ui.style()).show(ui, |ui| {
                    ui.set_width(150.0);
                    if ui.checkbox(&mut self.wander, "Wander around").changed() {
                        close = true;
                    }
                    if ui.button("Sit down").clicked() {
                        let now = self.start.elapsed().as_secs_f32();
                        self.state = State::Sit { until: now + 20.0 };
                        close = true;
                    }
                    if ui.button("Hop").clicked() {
                        self.happy_until = self.start.elapsed().as_secs_f32() + 1.6;
                        self.velocity = vec2(0.0, -620.0);
                        self.state = State::Falling;
                        close = true;
                    }
                    ui.separator();
                    ui.label(egui::RichText::new("Drop a .vrm file on the avatar to change it.").small());
                    ui.separator();
                    if ui.button("Quit").clicked() {
                        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    }
                });
            })
            .response;
        let clicked_elsewhere = ctx.input(|i| i.pointer.any_pressed())
            && !ctx.input(|i| i.pointer.interact_pos()).is_some_and(|p| response.rect.contains(p));
        if close || clicked_elsewhere || ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
            self.menu = None;
        }
    }

    fn welcome_ui(&mut self, ctx: &egui::Context) {
        egui::CentralPanel::default()
            .frame(egui::Frame::window(&ctx.style()).inner_margin(16.0))
            .show(ctx, |ui| {
                ui.heading("KitVR mascot");
                ui.add_space(8.0);
                ui.label("Drop a VRM avatar (.vrm) onto this window, or start it with:");
                ui.code("kitvr-mascot avatar.vrm");
                if let Some(message) = &self.message {
                    ui.add_space(8.0);
                    ui.colored_label(egui::Color32::from_rgb(230, 110, 110), message);
                }
                ui.add_space(12.0);
                if ui.button("Quit").clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
                let drag = ui.interact(ui.max_rect(), egui::Id::new("welcome-drag"), egui::Sense::drag());
                if drag.drag_started() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::StartDrag);
                }
            });
    }
}

impl eframe::App for MascotApp {
    fn clear_color(&self, _visuals: &egui::Visuals) -> [f32; 4] {
        [0.0; 4]
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        if let Some(path) = &self.vrm_path {
            storage.set_string(STORAGE_VRM, path.to_string_lossy().into_owned());
        }
        storage.set_string(STORAGE_WANDER, self.wander.to_string());
    }

    fn on_exit(&mut self, gl: Option<&eframe::glow::Context>) {
        if let (Some(gl), Ok(mut scene)) = (gl, self.scene.lock()) {
            if let Some((_, renderer)) = scene.renderer.take() {
                renderer.destroy(gl);
            }
        }
    }

    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let now_instant = Instant::now();
        let dt = now_instant.duration_since(self.last_frame).as_secs_f32().min(0.05);
        self.last_frame = now_instant;
        let now = self.start.elapsed().as_secs_f32();
        ctx.request_repaint_after(Duration::from_millis(16));

        let dropped: Option<PathBuf> = ctx.input(|i| i.raw.dropped_files.iter().find_map(|f| f.path.clone()));
        if let Some(path) = dropped {
            self.load(&path);
        }
        if let Some(error) = self.scene.lock().unwrap().error.take() {
            self.message = Some(format!("could not draw this avatar: {error}"));
            self.avatar = None;
        }

        if self.position.is_none() {
            if let Some(rect) = ctx.input(|i| i.viewport().outer_rect) {
                if self.avatar.is_some() {
                    // Drop in from above the bottom right of the work area.
                    let work = self.work_area(ctx);
                    self.position = Some(pos2(work.max.x - SIZE.x - 60.0, work.min.y.max(rect.min.y)));
                }
            }
        }

        if self.avatar.is_none() {
            if self.passthrough {
                ctx.send_viewport_cmd(egui::ViewportCommand::MousePassthrough(false));
                self.passthrough = false;
            }
            self.welcome_ui(ctx);
            return;
        }

        let ppp = ctx.pixels_per_point();
        let cursor = self
            .desktop
            .as_ref()
            .and_then(|d| d.cursor())
            .map(|c| (pos2(c.x / ppp, c.y / ppp), c.left_down));
        let hit = self.scene.lock().unwrap().hit;
        self.step(ctx, now, dt, cursor, hit);

        let position = self.position.unwrap_or(Pos2::ZERO);
        if self.sent_position.map_or(true, |sent| (sent - position).length() > 0.4) {
            ctx.send_viewport_cmd(egui::ViewportCommand::OuterPosition(position));
            self.sent_position = Some(position);
        }

        // Pose the avatar and hand it to the paint callback.
        let avatar = self.avatar.as_ref().unwrap();
        let world = avatar.skeleton.pose(&avatar.model, &pose::build(&self.motion));
        let local_cursor = cursor.map(|(c, _)| c - position.to_vec2());
        let probe = local_cursor
            .filter(|c| c.x >= 0.0 && c.y >= 0.0 && c.x < SIZE.x && c.y < SIZE.y)
            .map(|c| [(c.x * ppp) as i32, ((SIZE.y - c.y) * ppp) as i32 - 1]);
        {
            let mut scene = self.scene.lock().unwrap();
            scene.world = world;
            scene.expressions = self.expressions(now);
            scene.mvp = avatar.projection;
            scene.probe = probe;
            if probe.is_none() {
                scene.hit = false;
            }
        }

        egui::CentralPanel::default().frame(egui::Frame::none()).show(ctx, |ui| {
            let rect = ui.max_rect();
            let scene = Arc::clone(&self.scene);
            let callback = egui_glow::CallbackFn::new(move |_info, painter| {
                if let Ok(mut scene) = scene.lock() {
                    scene.paint(painter.gl());
                }
            });
            ui.painter().add(egui::PaintCallback { rect, callback: Arc::new(callback) });
        });
        self.menu_ui(ctx);

        // Only the avatar (and the menu) catch the mouse; the rest of the window lets clicks through.
        let busy = self.menu.is_some() || matches!(self.state, State::Pressed { .. } | State::Dragged { .. });
        let want_passthrough = !(hit || busy) && self.desktop.is_some();
        if want_passthrough != self.passthrough {
            ctx.send_viewport_cmd(egui::ViewportCommand::MousePassthrough(want_passthrough));
            self.passthrough = want_passthrough;
        }
    }
}

fn main() -> eframe::Result<()> {
    // Wayland does not let a client place its own window, stay on top, or see the cursor outside
    // its surfaces, which is everything a mascot does. Run through XWayland instead.
    #[cfg(target_os = "linux")]
    if std::env::var_os("DISPLAY").is_some() {
        std::env::remove_var("WAYLAND_DISPLAY");
    }
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("KitVR mascot")
            .with_app_id("kitvr-mascot")
            .with_inner_size(SIZE)
            .with_resizable(false)
            .with_decorations(false)
            .with_transparent(true)
            .with_always_on_top()
            .with_taskbar(false)
            .with_drag_and_drop(true),
        depth_buffer: 24,
        persist_window: false,
        ..Default::default()
    };
    eframe::run_native("kitvr-mascot", options, Box::new(|cc| Ok(Box::new(MascotApp::new(cc)))))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Loads a real avatar when `KITVR_TEST_VRM` points at one; skipped otherwise.
    #[test]
    fn loads_and_poses_a_real_vrm() {
        let Some(path) = std::env::var_os("KITVR_TEST_VRM") else { return };
        let model = vrm::load(&std::fs::read(path).unwrap()).unwrap();
        let skeleton = Skeleton::new(&model);
        let rest = skeleton.pose(&model, &pose::build(&Motion::default()));
        let (min, max) = Skeleton::bounds(&model, &rest);
        let height = max.y - min.y;
        println!("{} primitives, bounds {min} .. {max}, sit drop {}", model.primitives.len(), skeleton.sit_drop);
        assert!((0.5..3.0).contains(&height), "avatar height {height} m");
        assert!(min.y > -0.2 && min.y < 0.2, "feet near the floor: {}", min.y);
        // Facing +Z: the head's nose side is in front of the back of the head.
        let head = rest[model.meta.bones["head"]];
        let left_hand = rest[model.meta.bones["leftHand"]].w_axis;
        assert!(left_hand.x > 0.0, "left hand should be at +X: {left_hand}");
        assert!(left_hand.y < head.w_axis.y - 0.3, "arms should hang down: {left_hand}");
        assert!(model.meta.expressions.contains_key(&Expression::Blink));
        let sitting = skeleton.pose(&model, &pose::build(&Motion { sit: 1.0, sit_drop: skeleton.sit_drop, ..Motion::default() }));
        let (sit_min, _) = Skeleton::bounds(&model, &sitting);
        assert!(sit_min.y > -0.15, "sitting avatar stays on the floor: {}", sit_min.y);
    }
}
