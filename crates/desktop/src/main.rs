use std::collections::VecDeque;
use std::io::{BufRead, BufReader};
use std::net::UdpSocket;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::Duration;

use eframe::egui::{self, Align, Color32, Layout, RichText, Stroke};

const LOG_LIMIT: usize = 800;

const INK: Color32 = Color32::from_rgb(14, 20, 22);
const HEADER_FILL: Color32 = Color32::from_rgb(24, 33, 36);
const SURFACE: Color32 = Color32::from_rgb(22, 30, 33);
const RAISED: Color32 = Color32::from_rgb(28, 38, 41);
const SUNKEN: Color32 = Color32::from_rgb(11, 17, 19);
const LINE: Color32 = Color32::from_rgb(42, 54, 58);
const TEXT: Color32 = Color32::from_rgb(232, 237, 236);
const MUTED: Color32 = Color32::from_rgb(147, 162, 163);
const DIM: Color32 = Color32::from_rgb(130, 145, 143);
const AMBER: Color32 = Color32::from_rgb(242, 166, 90);
const AMBER_INK: Color32 = Color32::from_rgb(26, 18, 6);
const AMBER_TEXT: Color32 = Color32::from_rgb(245, 192, 138);
const GREEN: Color32 = Color32::from_rgb(98, 201, 142);
const GREEN_TEXT: Color32 = Color32::from_rgb(168, 232, 195);
const RED_TEXT: Color32 = Color32::from_rgb(243, 176, 169);
const ENCODERS: &[&str] = if cfg!(windows) {
    &["hevc_amf", "hevc_nvenc", "hevc_qsv", "libx265"]
} else {
    &["hevc_vaapi", "hevc_nvenc", "hevc_qsv", "libx265"]
};
const RESOLUTIONS: &[&str] = &["1920x1080", "2560x1440", "1280x720"];
const FRAME_RATES: &[u32] = &[60, 72, 90];
const RELEASE_API: &str =
    "https://api.github.com/repos/Crystal-Kitsune-Studios/KitVR/releases/latest";
const WELCOME_STORAGE_KEY: &str = "kitvr.desktop.welcome_completed";
const SETTINGS_STORAGE_KEY: &str = "kitvr.desktop.settings";

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1080.0, 720.0])
            .with_min_inner_size([900.0, 620.0]),
        ..Default::default()
    };
    eframe::run_native(
        "KitVR Desktop",
        options,
        Box::new(|cc| Ok(Box::new(KitVrApp::new(cc)))),
    )
}

struct KitVrApp {
    host_ip: String,
    source: String,
    file_path: String,
    encoder: String,
    resolution: String,
    fps: u32,
    bitrate_kbps: u32,
    audio_enabled: bool,
    audio_device: String,
    microphone_device: String,
    mouse_enabled: bool,
    audio_devices: Vec<String>,
    child: Option<Child>,
    log_tx: Sender<String>,
    log_rx: Receiver<String>,
    logs: VecDeque<String>,
    status: String,
    tutorial_open: bool,
    tutorial_completed: bool,
    tutorial_step: usize,
    update_rx: Receiver<ReleaseCheck>,
    update_offer: Option<UpdateOffer>,
    update_message: String,
    update_action_tx: Sender<UpdateAction>,
    update_action_rx: Receiver<UpdateAction>,
    restart_for_update: bool,
    update_in_progress: bool,
    console_open: bool,
    autoscroll: bool,
}

struct UpdateOffer {
    version: String,
    url: String,
    asset_name: String,
}

enum ReleaseCheck {
    Current(String),
    Available(UpdateOffer),
    Failed(String),
}

enum UpdateAction {
    Restarting,
    Failed(String),
}

#[derive(serde::Deserialize, serde::Serialize)]
#[serde(default)]
struct SavedSettings {
    source: String,
    file_path: String,
    encoder: String,
    resolution: String,
    fps: u32,
    bitrate_kbps: u32,
    audio_enabled: bool,
    audio_device: String,
    microphone_device: String,
}

impl Default for SavedSettings {
    fn default() -> Self {
        Self {
            source: "Test pattern".to_string(),
            file_path: String::new(),
            encoder: "libx265".to_string(),
            resolution: RESOLUTIONS[0].to_string(),
            fps: 60,
            bitrate_kbps: 40_000,
            audio_enabled: true,
            audio_device: "default".to_string(),
            microphone_device: String::new(),
        }
    }
}

impl KitVrApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let mut visuals = egui::Visuals::dark();
        visuals.panel_fill = INK;
        visuals.window_fill = SURFACE;
        visuals.extreme_bg_color = SUNKEN;
        visuals.faint_bg_color = Color32::from_rgb(18, 25, 27);
        visuals.window_stroke = Stroke::new(1.0_f32, LINE);
        visuals.widgets.noninteractive.bg_fill = SURFACE;
        visuals.widgets.noninteractive.fg_stroke = Stroke::new(1.0_f32, TEXT);
        visuals.widgets.inactive.bg_fill = RAISED;
        visuals.widgets.inactive.fg_stroke = Stroke::new(1.0_f32, TEXT);
        visuals.widgets.inactive.rounding = egui::Rounding::same(8.0);
        visuals.widgets.hovered.bg_fill = Color32::from_rgb(38, 50, 53);
        visuals.widgets.hovered.fg_stroke = Stroke::new(1.0_f32, TEXT);
        visuals.widgets.hovered.rounding = egui::Rounding::same(8.0);
        visuals.widgets.active.bg_fill = AMBER;
        visuals.widgets.active.fg_stroke = Stroke::new(1.0_f32, AMBER_INK);
        visuals.widgets.active.rounding = egui::Rounding::same(8.0);
        visuals.selection.bg_fill = Color32::from_rgba_unmultiplied(242, 166, 90, 60);
        visuals.selection.stroke = Stroke::new(1.0_f32, AMBER_TEXT);
        visuals.window_rounding = egui::Rounding::same(12.0);
        visuals.menu_rounding = egui::Rounding::same(8.0);
        cc.egui_ctx.set_visuals(visuals);
        let mut style = (*cc.egui_ctx.style()).clone();
        style.spacing.item_spacing = egui::vec2(10.0, 8.0);
        style.spacing.button_padding = egui::vec2(12.0, 7.0);
        style.spacing.interact_size.y = 26.0;
        cc.egui_ctx.set_style(style);

        let (log_tx, log_rx) = mpsc::channel();
        let (update_tx, update_rx) = mpsc::channel();
        let (update_action_tx, update_action_rx) = mpsc::channel();
        let tutorial_completed = cc
            .storage
            .and_then(|storage| storage.get_string(WELCOME_STORAGE_KEY))
            .is_some_and(|value| value == "true");
        let settings = cc
            .storage
            .and_then(|storage| storage.get_string(SETTINGS_STORAGE_KEY))
            .and_then(|value| serde_json::from_str::<SavedSettings>(&value).ok())
            .unwrap_or_default();
        let app = Self {
            host_ip: local_ip(),
            source: settings.source,
            file_path: settings.file_path,
            encoder: settings.encoder,
            resolution: settings.resolution,
            fps: settings.fps,
            bitrate_kbps: settings.bitrate_kbps,
            audio_enabled: settings.audio_enabled,
            audio_device: settings.audio_device,
            microphone_device: settings.microphone_device,
            mouse_enabled: false,
            audio_devices: Vec::new(),
            child: None,
            log_tx,
            log_rx,
            logs: VecDeque::new(),
            status: "Ready. Start KitVR Server, then connect from your Quest.".to_string(),
            tutorial_open: !tutorial_completed,
            tutorial_completed,
            tutorial_step: 0,
            update_rx,
            update_offer: None,
            update_message: "Checking for updates...".to_string(),
            update_action_tx,
            update_action_rx,
            restart_for_update: false,
            update_in_progress: false,
            console_open: true,
            autoscroll: true,
        };
        thread::spawn(move || {
            let _ = update_tx.send(check_latest_release());
        });
        app
    }

    fn push_log(&mut self, line: impl Into<String>) {
        self.logs.push_back(line.into());
        while self.logs.len() > LOG_LIMIT {
            self.logs.pop_front();
        }
    }

    fn start_server(&mut self) {
        if self.child.is_some() {
            return;
        }
        if self.source == "File" && self.file_path.trim().is_empty() {
            self.status = "Choose a media file first.".to_string();
            return;
        }

        let Some(executable) = find_server_executable() else {
            self.status = "kitvr-server was not found beside KitVR Desktop or on PATH.".to_string();
            self.push_log(self.status.clone());
            return;
        };

        let (width, height) = parse_resolution(&self.resolution);
        let source = match self.source.as_str() {
            "Desktop" => "desktop".to_string(),
            "File" => format!("file:{}", self.file_path.trim()),
            _ => "testpattern".to_string(),
        };
        let mut args = vec![
            "--source".to_string(),
            source,
            "--encoder".to_string(),
            self.encoder.clone(),
            "--width".to_string(),
            width.to_string(),
            "--height".to_string(),
            height.to_string(),
            "--fps".to_string(),
            self.fps.to_string(),
            "--bitrate-kbps".to_string(),
            self.bitrate_kbps.to_string(),
        ];
        if !self.audio_enabled {
            args.push("--no-audio".to_string());
        } else if self.audio_device != "default" {
            args.extend(["--audio-device".to_string(), self.audio_device.clone()]);
        }
        if !self.microphone_device.trim().is_empty() {
            args.extend([
                "--mic-device".to_string(),
                self.microphone_device.trim().to_string(),
            ]);
        }
        if self.mouse_enabled {
            args.push("--enable-mouse".to_string());
        }

        match Command::new(&executable)
            .args(&args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
        {
            Ok(mut child) => {
                self.push_log(format!(
                    "Launching {} {}",
                    executable.display(),
                    args.join(" ")
                ));
                self.status = "Server starting. On Wayland, approve the screen-sharing dialog.".to_string();
                if let Some(stdout) = child.stdout.take() {
                    pipe_logs(stdout, self.log_tx.clone());
                }
                if let Some(stderr) = child.stderr.take() {
                    pipe_logs(stderr, self.log_tx.clone());
                }
                self.child = Some(child);
            }
            Err(error) => {
                self.status = format!("Could not start server: {error}");
                self.push_log(self.status.clone());
            }
        }
    }

    fn stop_server(&mut self) {
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            match child.wait() {
                Ok(status) => self.push_log(format!("Server stopped ({status}).")),
                Err(error) => self.push_log(format!("Server stopped; wait failed: {error}")),
            }
        }
        self.status = "Server stopped.".to_string();
    }

    fn refresh_audio_devices(&mut self) {
        let Some(executable) = find_server_executable() else {
            self.status = "Build kitvr-server or put it beside KitVR Desktop.".to_string();
            return;
        };
        match Command::new(executable).arg("--list-audio").output() {
            Ok(output) if output.status.success() => {
                let lines = String::from_utf8_lossy(&output.stdout);
                let devices: Vec<String> = lines
                    .lines()
                    .filter_map(|line| {
                        line.split_once(": ")
                            .map(|(_, name)| name.trim().to_string())
                    })
                    .filter(|name| !name.starts_with("no devices found"))
                    .collect();
                self.audio_devices = devices;
                if self.audio_devices.is_empty() {
                    self.push_log("No audio devices reported. Check your audio backend.");
                } else {
                    self.push_log("Audio devices:");
                    for device in self.audio_devices.clone() {
                        self.push_log(format!("  {device}"));
                    }
                }
            }
            Ok(output) => self.push_log(format!(
                "Audio device listing failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            )),
            Err(error) => self.push_log(format!("Could not list audio devices: {error}")),
        }
    }

    fn update_server(&mut self) {
        while let Ok(line) = self.log_rx.try_recv() {
            if line.starts_with("listening: control TCP ") {
                self.status = format!("Server running. Enter {} in the Quest app.", self.host_ip);
            }
            self.push_log(line);
        }
        let exited = match self.child.as_mut() {
            Some(child) => match child.try_wait() {
                Ok(Some(status)) => Some(format!("Server exited ({status}).")),
                Ok(None) => None,
                Err(error) => Some(format!("Could not check server process: {error}")),
            },
            None => None,
        };
        if let Some(message) = exited {
            self.child = None;
            self.status = message.clone();
            self.push_log(message);
        }
        while let Ok(result) = self.update_rx.try_recv() {
            match result {
                ReleaseCheck::Current(version) => {
                    self.update_message = format!("Up to date · v{version}");
                    self.update_offer = None;
                }
                ReleaseCheck::Available(offer) => {
                    self.update_message = format!("Update available · v{}", offer.version);
                    self.update_offer = Some(offer);
                }
                ReleaseCheck::Failed(message) => {
                    self.update_message = message;
                    self.update_offer = None;
                }
            }
        }
        while let Ok(action) = self.update_action_rx.try_recv() {
            match action {
                UpdateAction::Restarting => {
                    self.update_message = "Applying update and restarting…".to_string();
                    self.restart_for_update = true;
                }
                UpdateAction::Failed(message) => {
                    self.update_message = message;
                    self.update_in_progress = false;
                }
            }
        }
    }

    fn start_update(&mut self) {
        if self.update_in_progress {
            return;
        }
        if self.child.is_some() {
            self.status = "Stop streaming before applying an update.".to_string();
            return;
        }
        let Some(offer) = self.update_offer.as_ref() else {
            return;
        };
        let Some(executable) = std::env::current_exe().ok() else {
            self.status = "Could not locate KitVR Desktop.".to_string();
            return;
        };
        let Some(install_dir) = executable.parent().map(PathBuf::from) else {
            self.status = "Could not locate the installation folder.".to_string();
            return;
        };
        let updater_name = if cfg!(windows) {
            "kitvr-updater.exe"
        } else {
            "kitvr-updater"
        };
        let updater = install_dir.join(updater_name);
        if !updater.is_file() {
            self.status =
                "Update helper is missing. Download the package from the release page.".to_string();
            let _ = webbrowser::open(&offer.url);
            return;
        }
        let mode = if cfg!(windows) { "windows" } else { "appimage" };
        let target = if cfg!(windows) {
            install_dir.clone()
        } else if let Some(path) = std::env::var_os("APPIMAGE") {
            PathBuf::from(path)
        } else {
            self.status = "For local builds, update from the GitHub release page.".to_string();
            let _ = webbrowser::open(&offer.url);
            return;
        };
        let version = offer.version.clone();
        let download_url = offer.url.clone();
        let asset_name = offer.asset_name.clone();
        let action_tx = self.update_action_tx.clone();
        self.update_in_progress = true;
        self.update_message = format!("Downloading v{version}…");
        self.status = format!("Downloading KitVR v{version}.");
        thread::spawn(move || {
            let result = download_update(&download_url, &asset_name).and_then(|package| {
                Command::new(updater)
                    .arg(mode)
                    .arg(std::process::id().to_string())
                    .arg(package)
                    .arg(target)
                    .spawn()
                    .map_err(|error| error.to_string())
            });
            match result {
                Ok(_) => {
                    let _ = action_tx.send(UpdateAction::Restarting);
                }
                Err(message) => {
                    let _ =
                        action_tx.send(UpdateAction::Failed(format!("Update failed: {message}")));
                }
            }
        });
    }

    fn close_tutorial(&mut self) {
        self.tutorial_open = false;
        self.tutorial_completed = true;
    }
}

impl eframe::App for KitVrApp {
    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        storage.set_string(WELCOME_STORAGE_KEY, self.tutorial_completed.to_string());
        let settings = SavedSettings {
            source: self.source.clone(),
            file_path: self.file_path.clone(),
            encoder: self.encoder.clone(),
            resolution: self.resolution.clone(),
            fps: self.fps,
            bitrate_kbps: self.bitrate_kbps,
            audio_enabled: self.audio_enabled,
            audio_device: self.audio_device.clone(),
            microphone_device: self.microphone_device.clone(),
        };
        if let Ok(value) = serde_json::to_string(&settings) {
            storage.set_string(SETTINGS_STORAGE_KEY, value);
        }
    }

    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.update_server();
        if self.restart_for_update {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            return;
        }
        ctx.request_repaint_after(Duration::from_millis(150));

        egui::TopBottomPanel::top("header")
            .frame(
                egui::Frame::default()
                    .fill(HEADER_FILL)
                    .stroke(Stroke::new(1.0_f32, LINE))
                    .inner_margin(egui::Margin::symmetric(20.0, 14.0)),
            )
            .show(ctx, |ui| self.header(ui));

        if self.console_open {
            self.show_console(ctx);
        }

        egui::CentralPanel::default()
            .frame(
                egui::Frame::default()
                    .fill(INK)
                    .inner_margin(egui::Margin::symmetric(20.0, 18.0)),
            )
            .show(ctx, |ui| self.body(ui));

        if self.tutorial_open {
            self.tutorial_window(ctx);
        }
    }
}

impl KitVrApp {
    fn show_console(&mut self, ctx: &egui::Context) {
        egui::TopBottomPanel::bottom("console")
            .resizable(true)
            .default_height(206.0)
            .min_height(132.0)
            .max_height(420.0)
            .frame(
                egui::Frame::default()
                    .fill(INK)
                    .inner_margin(egui::Margin::symmetric(20.0, 14.0)),
            )
            .show(ctx, |ui| self.console_panel(ui));
    }

    fn header(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            let (mark_rect, _) =
                ui.allocate_exact_size(egui::vec2(30.0, 30.0), egui::Sense::hover());
            ui.painter()
                .rect_filled(mark_rect, egui::Rounding::same(8.0), AMBER);
            let center = mark_rect.center();
            ui.painter().add(egui::Shape::convex_polygon(
                vec![
                    egui::pos2(center.x, center.y - 7.5),
                    egui::pos2(center.x + 7.5, center.y + 6.0),
                    egui::pos2(center.x - 7.5, center.y + 6.0),
                ],
                AMBER_INK,
                Stroke::NONE,
            ));

            ui.add_space(2.0);
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 1.0;
                ui.label(RichText::new("KitVR").size(17.0).strong().color(TEXT));
                ui.label(
                    RichText::new("PC to Quest streaming")
                        .size(11.5)
                        .color(MUTED),
                );
            });

            ui.add_space(8.0);
            self.status_pill(ui);

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.label(
                    RichText::new(format!("v{}", app_version()))
                        .size(11.0)
                        .color(DIM),
                );
                let log_label = if self.console_open { "Hide log" } else { "Log" };
                if ui.button(log_label).clicked() {
                    self.console_open = !self.console_open;
                }
                if ui.button("Tutorial").clicked() {
                    self.tutorial_open = true;
                    self.tutorial_step = 0;
                }
                if let Some(version) = self.update_offer.as_ref().map(|offer| offer.version.clone())
                {
                    let label = if self.update_in_progress {
                        "Updating…"
                    } else {
                        "Get update"
                    };
                    if ui
                        .add_enabled(!self.update_in_progress, egui::Button::new(label))
                        .clicked()
                    {
                        self.start_update();
                    }
                    ui.label(
                        RichText::new(format!("Update · v{version}"))
                            .size(11.5)
                            .color(GREEN_TEXT),
                    );
                } else {
                    let message = self.update_message.clone();
                    ui.label(RichText::new(message).size(11.0).color(DIM));
                }
            });
        });
    }

    fn status_pill(&self, ui: &mut egui::Ui) {
        let running = self.child.is_some();
        let (fill, border, dot, text, text_color) = if running {
            (
                Color32::from_rgba_unmultiplied(98, 201, 142, 28),
                Color32::from_rgba_unmultiplied(98, 201, 142, 95),
                GREEN,
                "Streaming",
                GREEN_TEXT,
            )
        } else {
            (
                Color32::from_rgba_unmultiplied(147, 162, 163, 16),
                Color32::from_rgba_unmultiplied(147, 162, 163, 60),
                MUTED,
                "Idle",
                MUTED,
            )
        };
        egui::Frame::default()
            .fill(fill)
            .stroke(Stroke::new(1.0_f32, border))
            .rounding(egui::Rounding::same(999.0))
            .inner_margin(egui::Margin::symmetric(11.0, 5.0))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 7.0;
                    let (dot_rect, _) =
                        ui.allocate_exact_size(egui::vec2(7.0, 7.0), egui::Sense::hover());
                    ui.painter().circle_filled(dot_rect.center(), 3.5, dot);
                    ui.label(RichText::new(text).size(12.0).strong().color(text_color));
                });
            });
    }

    fn body(&mut self, ui: &mut egui::Ui) {
        let available = ui.available_size();
        let right_width = (available.x * 0.36).clamp(300.0, 372.0);
        let gap = 18.0;
        let left_width = (available.x - right_width - gap).max(300.0);
        ui.horizontal_top(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            ui.allocate_ui_with_layout(
                egui::vec2(left_width, available.y),
                Layout::top_down(Align::Min),
                |ui| {
                    egui::ScrollArea::vertical()
                        .auto_shrink([false, false])
                        .show(ui, |ui| self.stream_card(ui, available.y));
                },
            );
            ui.add_space(gap);
            ui.allocate_ui_with_layout(
                egui::vec2(right_width, available.y),
                Layout::top_down(Align::Min),
                |ui| self.connect_card(ui, available.y),
            );
        });
    }

    fn stream_card(&mut self, ui: &mut egui::Ui, height: f32) {
        let running = self.child.is_some();
        egui::Frame::default()
            .fill(SURFACE)
            .stroke(Stroke::new(1.0_f32, LINE))
            .rounding(egui::Rounding::same(12.0))
            .inner_margin(egui::Margin::same(16.0))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.set_min_height((height - 34.0).max(140.0));
                ui.horizontal(|ui| {
                    section_label(ui, "Stream setup");
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        tag(ui, "hevc · low latency");
                    });
                });
                ui.add_space(12.0);

                ui.add_enabled_ui(!running, |ui| {
                    let source = self.source.clone();
                    let encoder = self.encoder.clone();
                    ui.columns(2, |columns| {
                        field(&mut columns[0], "VIDEO SOURCE", |ui| {
                            egui::ComboBox::from_id_salt("source")
                                .selected_text(RichText::new(source).size(13.5).color(TEXT))
                                .width(ui.available_width())
                                .show_ui(ui, |ui| {
                                    for option in ["Test pattern", "Desktop", "File"] {
                                        ui.selectable_value(
                                            &mut self.source,
                                            option.to_string(),
                                            option,
                                        );
                                    }
                                });
                        });
                        field(&mut columns[1], "ENCODER", |ui| {
                            egui::ComboBox::from_id_salt("encoder")
                                .selected_text(RichText::new(encoder).size(13.5).color(TEXT))
                                .width(ui.available_width())
                                .show_ui(ui, |ui| {
                                    for option in ENCODERS {
                                        ui.selectable_value(
                                            &mut self.encoder,
                                            (*option).to_string(),
                                            *option,
                                        );
                                    }
                                });
                        });
                    });

                    if self.source == "File" {
                        ui.add_space(8.0);
                        field(ui, "MEDIA FILE", |ui| {
                            ui.add(
                                egui::TextEdit::singleline(&mut self.file_path)
                                    .hint_text("Path to a video file")
                                    .desired_width(ui.available_width())
                                    .frame(false),
                            );
                        });
                    }

                    ui.add_space(8.0);
                    let resolution = self.resolution.clone();
                    ui.columns(2, |columns| {
                        field(&mut columns[0], "RESOLUTION", |ui| {
                            egui::ComboBox::from_id_salt("resolution")
                                .selected_text(RichText::new(resolution).size(13.5).color(TEXT))
                                .width(ui.available_width())
                                .show_ui(ui, |ui| {
                                    for option in RESOLUTIONS {
                                        ui.selectable_value(
                                            &mut self.resolution,
                                            (*option).to_string(),
                                            *option,
                                        );
                                    }
                                });
                        });
                        field(&mut columns[1], "FRAME RATE", |ui| {
                            egui::ComboBox::from_id_salt("fps")
                                .selected_text(
                                    RichText::new(format!("{} fps", self.fps))
                                        .size(13.5)
                                        .color(TEXT),
                                )
                                .width(ui.available_width())
                                .show_ui(ui, |ui| {
                                    for fps in FRAME_RATES {
                                        ui.selectable_value(
                                            &mut self.fps,
                                            *fps,
                                            format!("{fps} fps"),
                                        );
                                    }
                                });
                        });
                    });

                    ui.add_space(12.0);
                    ui.horizontal(|ui| {
                        section_label(ui, "Bitrate");
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            ui.label(
                                RichText::new(format!(
                                    "{:.1} Mbps",
                                    self.bitrate_kbps as f32 / 1000.0
                                ))
                                .size(13.0)
                                .strong()
                                .color(TEXT),
                            );
                        });
                    });
                    ui.spacing_mut().slider_width = (ui.available_width() - 64.0).max(120.0);
                    ui.add(
                        egui::Slider::new(&mut self.bitrate_kbps, 5_000..=120_000)
                            .step_by(5_000.0)
                            .suffix(" kbps"),
                    );

                    ui.add_space(12.0);
                    ui.horizontal(|ui| {
                        section_label(ui, "Audio");
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            if ui.button("Rescan").clicked() {
                                self.refresh_audio_devices();
                            }
                        });
                    });
                    ui.add_space(6.0);
                    let mut audio_enabled = self.audio_enabled;
                    option_row(
                        ui,
                        "Send PC audio to the headset",
                        "Devices are matched by name, so virtual cables work too",
                        &mut audio_enabled,
                    );
                    self.audio_enabled = audio_enabled;

                    if self.audio_enabled {
                        ui.add_space(8.0);
                        let selected = self.audio_device.clone();
                        let devices = self.audio_devices.clone();
                        let selected_text = if selected == "default" {
                            "System default output".to_string()
                        } else {
                            selected
                        };
                        field(ui, "AUDIO DEVICE", |ui| {
                            egui::ComboBox::from_id_salt("audio")
                                .selected_text(RichText::new(selected_text).size(13.5).color(TEXT))
                                .width(ui.available_width())
                                .show_ui(ui, |ui| {
                                    ui.selectable_value(
                                        &mut self.audio_device,
                                        "default".to_string(),
                                        "System default output",
                                    );
                                    for device in &devices {
                                        ui.selectable_value(
                                            &mut self.audio_device,
                                            device.clone(),
                                            device,
                                        );
                                    }
                                });
                        });
                    }

                    ui.add_space(8.0);
                    let mut microphone = self.microphone_device.clone();
                    field(ui, "PC MICROPHONE DESTINATION", |ui| {
                        ui.add(
                            egui::TextEdit::singleline(&mut microphone)
                                .hint_text("Leave empty to keep the microphone off")
                                .desired_width(ui.available_width())
                                .frame(false),
                        );
                    });
                    self.microphone_device = microphone;
                    ui.add_space(12.0);
                    section_label(ui, "Mouse control");
                    ui.add_space(6.0);
                    option_row(
                        ui,
                        "Enable hand/controller mouse",
                        "Right hand moves; trigger or pinch clicks, grip right-clicks. Trusted networks only: connections are unauthenticated.",
                        &mut self.mouse_enabled,
                    );
                });

                ui.add_space(14.0);
                let (label, fill, border, text_color) = if running {
                    (
                        "Stop streaming",
                        Color32::from_rgba_unmultiplied(228, 115, 106, 26),
                        Color32::from_rgba_unmultiplied(228, 115, 106, 120),
                        RED_TEXT,
                    )
                } else {
                    ("Start streaming", AMBER, AMBER, AMBER_INK)
                };
                let button = egui::Button::new(RichText::new(label).size(15.0).strong().color(text_color))
                    .fill(fill)
                    .stroke(Stroke::new(1.0_f32, border))
                    .min_size(egui::vec2(ui.available_width(), 46.0));
                if ui.add(button).clicked() {
                    if running {
                        self.stop_server();
                    } else {
                        self.start_server();
                    }
                }

                ui.add_space(10.0);
                let note = if running {
                    "Stop the stream before changing the source, encoder or resolution."
                } else {
                    "Test pattern needs no capture, so it is the quickest way to prove the link works."
                };
                ui.label(RichText::new(note).size(11.5).color(DIM));
                ui.add_space(8.0);
                ui.label(
                    RichText::new(
                        "Desktop capture uses gdigrab on Windows, x11grab on X11, and screen sharing via PipeWire and GStreamer on Wayland. On Wayland, choose a screen or window in the sharing dialog.",
                    )
                    .size(11.5)
                    .color(DIM),
                );
            });
    }

    fn connect_card(&mut self, ui: &mut egui::Ui, height: f32) {
        egui::Frame::default()
            .fill(SURFACE)
            .stroke(Stroke::new(1.0_f32, LINE))
            .rounding(egui::Rounding::same(12.0))
            .inner_margin(egui::Margin::same(16.0))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.set_min_height((height - 34.0).max(140.0));
                ui.horizontal(|ui| {
                    section_label(ui, "Connect from Quest");
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if ui.button("Refresh").clicked() {
                            self.host_ip = local_ip();
                        }
                    });
                });
                ui.add_space(10.0);

                let address = self.host_ip.clone();
                egui::Frame::default()
                    .fill(SUNKEN)
                    .stroke(Stroke::new(1.0_f32, LINE))
                    .rounding(egui::Rounding::same(10.0))
                    .inner_margin(egui::Margin::symmetric(14.0, 12.0))
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.horizontal(|ui| {
                            ui.label(RichText::new(&address).monospace().size(21.0).color(TEXT));
                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                if ui.button("Copy").clicked() {
                                    ui.output_mut(|output| output.copied_text = address.clone());
                                }
                            });
                        });
                    });

                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    tag(ui, "TCP 9943 control");
                    tag(ui, "UDP 9944 video");
                });

                ui.add_space(12.0);
                step_row(ui, 1, "Open KitVR in the headset and pick this PC.");
                step_row(ui, 2, "Press Connect while the server is running here.");
                step_row(ui, 3, "The stream opens as a flat panel 2 m ahead.");

                ui.add_space(10.0);
                ui.separator();
                ui.add_space(8.0);
                ui.label(
                    RichText::new(
                        "If this PC is missing in the headset list, check that both devices are on the same network and that TCP 9943 plus UDP 9944 are allowed through the firewall.",
                    )
                    .size(11.5)
                    .color(DIM),
                );

                ui.add_space(12.0);
                section_label(ui, "Limits in this build");
                ui.add_space(6.0);
                for line in [
                    "About one extra frame of latency",
                    "Fixed GOP, keyframe requests are not honored",
                    "Mouse control is opt-in and unauthenticated",
                    "Audio is raw PCM, not compressed",
                ] {
                    ui.label(RichText::new(format!("- {line}")).size(11.5).color(MUTED));
                }
            });
    }

    fn console_panel(&mut self, ui: &mut egui::Ui) {
        let status = self.status.clone();
        ui.horizontal(|ui| {
            section_label(ui, "Server output");
            ui.add_space(4.0);
            ui.label(RichText::new(status).size(11.5).color(MUTED));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if ui.button("Clear").clicked() {
                    self.logs.clear();
                }
                if ui.button("Copy").clicked() {
                    let text = self.logs.iter().cloned().collect::<Vec<_>>().join("\n");
                    ui.output_mut(|output| output.copied_text = text);
                }
                let label = if self.autoscroll {
                    "Autoscroll on"
                } else {
                    "Autoscroll off"
                };
                if ui.button(label).clicked() {
                    self.autoscroll = !self.autoscroll;
                }
            });
        });
        ui.add_space(8.0);

        egui::Frame::default()
            .fill(SUNKEN)
            .stroke(Stroke::new(1.0_f32, LINE))
            .rounding(egui::Rounding::same(12.0))
            .inner_margin(egui::Margin::symmetric(14.0, 10.0))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                let mut area = egui::ScrollArea::vertical()
                    .auto_shrink([false, false])
                    .min_scrolled_height(0.0);
                if self.autoscroll {
                    area = area.stick_to_bottom(true);
                }
                area.show(ui, |ui| {
                    if self.logs.is_empty() {
                        ui.label(
                            RichText::new("Server messages will appear here.")
                                .size(11.5)
                                .color(DIM),
                        );
                    }
                    for line in &self.logs {
                        let lower = line.to_ascii_lowercase();
                        let color = if lower.contains("error")
                            || lower.contains("failed")
                            || lower.contains("could not")
                        {
                            RED_TEXT
                        } else if lower.contains("warn") {
                            AMBER_TEXT
                        } else {
                            Color32::from_rgb(147, 166, 165)
                        };
                        ui.label(RichText::new(line).monospace().size(11.5).color(color));
                    }
                });
            });
    }

    fn tutorial_window(&mut self, ctx: &egui::Context) {
        let mut open = self.tutorial_open;
        egui::Window::new("Welcome to KitVR")
            .id(egui::Id::new("kitvr_welcome"))
            .anchor(egui::Align2::CENTER_CENTER, egui::Vec2::ZERO)
            .collapsible(false)
            .resizable(false)
            .open(&mut open)
            .show(ctx, |ui| {
                ui.set_width(520.0);
                ui.label(
                    RichText::new(format!("Getting started  ·  {}/3", self.tutorial_step + 1))
                        .size(11.0)
                        .strong()
                        .color(AMBER_TEXT),
                );
                ui.add_space(10.0);
                match self.tutorial_step {
                    0 => {
                        ui.heading("Your PC, in your Quest");
                        ui.add_space(8.0);
                        ui.label("KitVR sends your PC's video and audio to the headset over your local network.");
                        ui.add_space(12.0);
                        ui.label("You'll need FFmpeg on the PC, and both devices on the same network.");
                    }
                    1 => {
                        ui.heading("Prepare the PC");
                        ui.add_space(8.0);
                        ui.label("Install FFmpeg and allow KitVR through your firewall on TCP 9943 and UDP 9944.");
                        ui.add_space(8.0);
                        ui.label("For a first test, keep Test pattern and the default software encoder selected.");
                    }
                    _ => {
                        ui.heading("Connect the Quest");
                        ui.add_space(8.0);
                        ui.label(format!(
                            "On the Quest, open KitVR and enter this PC address: {}",
                            self.host_ip
                        ));
                        ui.add_space(8.0);
                        ui.label("Start the server here, then press Connect in the headset app.");
                    }
                }
                ui.add_space(20.0);
                ui.separator();
                ui.horizontal(|ui| {
                    if self.tutorial_step > 0 && ui.button("Back").clicked() {
                        self.tutorial_step -= 1;
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if self.tutorial_step < 2 {
                            if ui.button("Next").clicked() {
                                self.tutorial_step += 1;
                            }
                            if ui.button("Skip").clicked() {
                                self.close_tutorial();
                            }
                        } else {
                            if ui.button("Start test stream").clicked() {
                                self.source = "Test pattern".to_string();
                                self.close_tutorial();
                                self.start_server();
                            }
                            if ui.button("Done").clicked() {
                                self.close_tutorial();
                            }
                        }
                    });
                });
            });
        self.tutorial_open = open && self.tutorial_open;
        if !self.tutorial_open {
            self.tutorial_completed = true;
        }
    }
}

fn section_label(ui: &mut egui::Ui, text: &str) {
    ui.label(
        RichText::new(text.to_uppercase())
            .size(10.5)
            .strong()
            .color(MUTED),
    );
}

fn tag(ui: &mut egui::Ui, text: &str) {
    egui::Frame::default()
        .fill(RAISED)
        .stroke(Stroke::new(1.0_f32, LINE))
        .rounding(egui::Rounding::same(6.0))
        .inner_margin(egui::Margin::symmetric(8.0, 4.0))
        .show(ui, |ui| {
            ui.label(RichText::new(text).monospace().size(10.5).color(MUTED));
        });
}

fn field(ui: &mut egui::Ui, label: &str, add: impl FnOnce(&mut egui::Ui)) {
    egui::Frame::default()
        .fill(RAISED)
        .stroke(Stroke::new(1.0_f32, LINE))
        .rounding(egui::Rounding::same(8.0))
        .inner_margin(egui::Margin::symmetric(10.0, 7.0))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(RichText::new(label).size(9.5).strong().color(DIM));
            add(ui);
        });
}

fn option_row(ui: &mut egui::Ui, title: &str, detail: &str, value: &mut bool) {
    egui::Frame::default()
        .fill(RAISED)
        .stroke(Stroke::new(1.0_f32, LINE))
        .rounding(egui::Rounding::same(10.0))
        .inner_margin(egui::Margin::symmetric(14.0, 10.0))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 2.0;
                    ui.label(RichText::new(title).size(13.0).color(TEXT));
                    ui.label(RichText::new(detail).size(11.0).color(MUTED));
                });
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    switch(ui, value);
                });
            });
        });
}

fn switch(ui: &mut egui::Ui, value: &mut bool) {
    let (rect, response) = ui.allocate_exact_size(egui::vec2(40.0, 22.0), egui::Sense::click());
    if response.clicked() {
        *value = !*value;
    }
    let track = if *value {
        AMBER
    } else {
        Color32::from_rgb(51, 64, 63)
    };
    ui.painter().rect_filled(rect, egui::Rounding::same(11.0), track);
    let knob_x = if *value {
        rect.right() - 11.0
    } else {
        rect.left() + 11.0
    };
    let knob = if *value {
        AMBER_INK
    } else {
        Color32::from_rgb(138, 153, 155)
    };
    ui.painter()
        .circle_filled(egui::pos2(knob_x, rect.center().y), 8.0, knob);
}

fn step_row(ui: &mut egui::Ui, number: u8, text: &str) {
    ui.horizontal_top(|ui| {
        let (rect, _) = ui.allocate_exact_size(egui::vec2(19.0, 19.0), egui::Sense::hover());
        let center = rect.center();
        ui.painter()
            .circle_filled(center, 9.5, Color32::from_rgb(46, 37, 26));
        ui.painter().circle_stroke(
            center,
            9.5,
            Stroke::new(1.0_f32, Color32::from_rgb(122, 88, 52)),
        );
        ui.painter().text(
            center,
            egui::Align2::CENTER_CENTER,
            number.to_string(),
            egui::FontId::proportional(11.0),
            AMBER_TEXT,
        );
        ui.add(
            egui::Label::new(
                RichText::new(text)
                    .size(12.5)
                    .color(Color32::from_rgb(203, 214, 212)),
            )
            .wrap(),
        );
        ui.add_space(2.0);
    });
}
impl Drop for KitVrApp {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn pipe_logs<R: std::io::Read + Send + 'static>(pipe: R, sender: Sender<String>) {
    thread::spawn(move || {
        for line in BufReader::new(pipe).lines() {
            match line {
                Ok(line) => {
                    if sender.send(line).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    });
}

fn find_server_executable() -> Option<PathBuf> {
    let name = if cfg!(windows) {
        "kitvr-server.exe"
    } else {
        "kitvr-server"
    };
    if let Some(path) = std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|dir| dir.join(name)))
        .filter(|path| path.is_file())
    {
        return Some(path);
    }
    std::env::var_os("PATH")?
        .to_string_lossy()
        .split(if cfg!(windows) { ';' } else { ':' })
        .map(|dir| PathBuf::from(dir).join(name))
        .find(|path| path.is_file())
}

fn local_ip() -> String {
    UdpSocket::bind("0.0.0.0:0")
        .and_then(|socket| {
            socket.connect("192.0.2.1:9")?;
            socket.local_addr()
        })
        .map(|address| address.ip().to_string())
        .unwrap_or_else(|_| "127.0.0.1".to_string())
}

fn parse_resolution(value: &str) -> (u32, u32) {
    let Some((width, height)) = value.split_once('x') else {
        return (1920, 1080);
    };
    match (width.parse(), height.parse()) {
        (Ok(width), Ok(height)) => (width, height),
        _ => (1920, 1080),
    }
}

fn app_version() -> String {
    let version = option_env!("KITVR_DESKTOP_VERSION")
        .unwrap_or(env!("CARGO_PKG_VERSION"))
        .trim_start_matches('v');
    if parse_version(version).is_some() {
        version.to_string()
    } else {
        env!("CARGO_PKG_VERSION").to_string()
    }
}

fn parse_version(version: &str) -> Option<[u64; 3]> {
    let mut parts = version.split('.');
    let parsed = [
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
    ];
    if parts.next().is_some() {
        return None;
    }
    Some(parsed)
}

fn is_newer_version(latest: &str, current: &str) -> bool {
    match (parse_version(latest), parse_version(current)) {
        (Some(latest), Some(current)) => latest > current,
        _ => false,
    }
}

fn check_latest_release() -> ReleaseCheck {
    let current_version = app_version();
    let response = match ureq::get(RELEASE_API)
        .set("Accept", "application/vnd.github+json")
        .set("User-Agent", "KitVR-Desktop")
        .call()
    {
        Ok(response) => response,
        Err(_) => return ReleaseCheck::Failed("Update check unavailable".to_string()),
    };
    let release: serde_json::Value = match response.into_json() {
        Ok(release) => release,
        Err(_) => return ReleaseCheck::Failed("Could not read the latest release".to_string()),
    };

    let Some(tag) = release.get("tag_name").and_then(|value| value.as_str()) else {
        return ReleaseCheck::Failed("Could not read the latest release".to_string());
    };
    let latest_version = tag.trim_start_matches('v');
    if parse_version(latest_version).is_none() {
        return ReleaseCheck::Failed("Latest release has an invalid version".to_string());
    }
    if parse_version(&current_version).is_none() {
        return ReleaseCheck::Failed("Installed version is invalid".to_string());
    }
    if !is_newer_version(latest_version, &current_version) {
        return ReleaseCheck::Current(current_version);
    }

    let asset_name = if cfg!(windows) {
        "KitVR-windows-x86_64.zip"
    } else {
        "KitVR-linux-x86_64.AppImage"
    };
    let asset_url = release
        .get("assets")
        .and_then(|assets| assets.as_array())
        .and_then(|assets| {
            assets.iter().find_map(|asset| {
                (asset.get("name")?.as_str()? == asset_name)
                    .then(|| asset.get("browser_download_url")?.as_str())
                    .flatten()
            })
        });
    let Some(url) = asset_url else {
        return ReleaseCheck::Failed("No update package found for this platform".to_string());
    };
    ReleaseCheck::Available(UpdateOffer {
        version: latest_version.to_string(),
        url: url.to_string(),
        asset_name: asset_name.to_string(),
    })
}

fn download_update(url: &str, asset_name: &str) -> Result<PathBuf, String> {
    let response = ureq::get(url)
        .set("User-Agent", "KitVR-Desktop")
        .call()
        .map_err(|error| error.to_string())?;
    let path =
        std::env::temp_dir().join(format!("kitvr-update-{}-{asset_name}", std::process::id()));
    let mut file = std::fs::File::create(&path).map_err(|error| error.to_string())?;
    std::io::copy(&mut response.into_reader(), &mut file).map_err(|error| error.to_string())?;
    if std::fs::metadata(&path)
        .map_err(|error| error.to_string())?
        .len()
        == 0
    {
        return Err("Downloaded update package was empty".to_string());
    }
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_app() -> KitVrApp {
        let (log_tx, log_rx) = mpsc::channel();
        let (_, update_rx) = mpsc::channel();
        let (update_action_tx, update_action_rx) = mpsc::channel();
        let settings = SavedSettings::default();
        KitVrApp {
            host_ip: String::new(),
            source: settings.source,
            file_path: settings.file_path,
            encoder: settings.encoder,
            resolution: settings.resolution,
            fps: settings.fps,
            bitrate_kbps: settings.bitrate_kbps,
            audio_enabled: settings.audio_enabled,
            audio_device: settings.audio_device,
            microphone_device: settings.microphone_device,
            mouse_enabled: false,
            audio_devices: Vec::new(),
            child: None,
            log_tx,
            log_rx,
            logs: VecDeque::new(),
            status: "Ready".to_string(),
            tutorial_open: false,
            tutorial_completed: true,
            tutorial_step: 0,
            update_rx,
            update_offer: None,
            update_message: String::new(),
            update_action_tx,
            update_action_rx,
            restart_for_update: false,
            update_in_progress: false,
            console_open: true,
            autoscroll: true,
        }
    }

    #[test]
    fn console_does_not_grow_across_frames() {
        for autoscroll in [false, true] {
            for count in [0, 100] {
                for initial_height in [132.0, 206.0, 300.0] {
                    let ctx = egui::Context::default();
                    ctx.data_mut(|data| {
                        data.insert_persisted(
                            egui::Id::new("console"),
                            egui::containers::panel::PanelState {
                                rect: egui::Rect::from_min_size(
                                    egui::Pos2::ZERO,
                                    egui::vec2(900.0, initial_height),
                                ),
                            },
                        );
                    });
                    let mut style = (*ctx.style()).clone();
                    style.spacing.item_spacing = egui::vec2(10.0, 8.0);
                    style.spacing.button_padding = egui::vec2(12.0, 7.0);
                    style.spacing.interact_size.y = 26.0;
                    ctx.set_style(style);
                    let mut app = test_app();
                    app.autoscroll = autoscroll;
                    app.logs = (0..count).map(|i| format!("Log line {i}")).collect();
                    for _ in 0..30 {
                        let input = egui::RawInput {
                            screen_rect: Some(egui::Rect::from_min_size(
                                egui::Pos2::ZERO,
                                egui::vec2(900.0, 620.0),
                            )),
                            ..Default::default()
                        };
                        let _ = ctx.run(input, |ctx| app.show_console(ctx));
                    }
                    let height =
                        egui::containers::panel::PanelState::load(&ctx, egui::Id::new("console"))
                            .unwrap()
                            .size()
                            .y;
                    assert!(
                        (height - initial_height).abs() <= 1.0,
                        "console changed from {initial_height} to {height}"
                    );
                }
            }
        }
    }

    #[test]
    fn parses_three_part_versions() {
        assert_eq!(parse_version("0.1.4"), Some([0, 1, 4]));
        assert_eq!(parse_version("v1.2.3"), None);
        assert_eq!(parse_version("1.2"), None);
    }

    #[test]
    fn compares_versions_numerically() {
        assert!(is_newer_version("0.1.10", "0.1.9"));
        assert!(!is_newer_version("0.1.4", "0.1.4"));
        assert!(!is_newer_version("main", "0.1.4"));
    }
}
