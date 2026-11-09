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
const ENCODERS: &[&str] = if cfg!(windows) {
    &["hevc_amf", "hevc_nvenc", "hevc_qsv", "libx265"]
} else {
    &["hevc_vaapi", "hevc_nvenc", "hevc_qsv", "libx265"]
};
const RESOLUTIONS: &[&str] = &["1920x1080", "2560x1440", "1280x720"];
const FRAME_RATES: &[u32] = &[60, 72, 90];

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1000.0, 720.0])
            .with_min_inner_size([820.0, 600.0]),
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
    audio_devices: Vec<String>,
    child: Option<Child>,
    log_tx: Sender<String>,
    log_rx: Receiver<String>,
    logs: VecDeque<String>,
    status: String,
}

impl KitVrApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let mut visuals = egui::Visuals::dark();
        visuals.panel_fill = Color32::from_rgb(17, 25, 27);
        visuals.window_fill = Color32::from_rgb(20, 29, 31);
        visuals.extreme_bg_color = Color32::from_rgb(10, 16, 18);
        visuals.widgets.inactive.bg_fill = Color32::from_rgb(35, 49, 50);
        visuals.widgets.hovered.bg_fill = Color32::from_rgb(50, 72, 67);
        visuals.widgets.active.bg_fill = Color32::from_rgb(83, 171, 130);
        visuals.selection.bg_fill = Color32::from_rgb(83, 171, 130);
        visuals.selection.stroke = Stroke::new(1.0_f32, Color32::WHITE);
        cc.egui_ctx.set_visuals(visuals);

        let (log_tx, log_rx) = mpsc::channel();
        Self {
            host_ip: local_ip(),
            source: "Test pattern".to_string(),
            file_path: String::new(),
            encoder: "libx265".to_string(),
            resolution: RESOLUTIONS[0].to_string(),
            fps: 60,
            bitrate_kbps: 40_000,
            audio_enabled: true,
            audio_device: "default".to_string(),
            microphone_device: String::new(),
            audio_devices: Vec::new(),
            child: None,
            log_tx,
            log_rx,
            logs: VecDeque::new(),
            status: "Ready. Start KitVR Server, then connect from your Quest.".to_string(),
        }
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
                self.status = format!("Server running. Enter {} in the Quest app.", self.host_ip);
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
    }
}

impl eframe::App for KitVrApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.update_server();
        ctx.request_repaint_after(Duration::from_millis(150));

        egui::TopBottomPanel::top("header")
            .frame(egui::Frame::default().inner_margin(egui::Margin::symmetric(26.0, 18.0)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.label(
                            RichText::new("KITVR")
                                .size(25.0)
                                .strong()
                                .color(Color32::WHITE),
                        );
                        ui.label(
                            RichText::new("PC STREAMING CONSOLE")
                                .size(11.0)
                                .color(muted()),
                        );
                    });
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        let (dot, text) = if self.child.is_some() {
                            (Color32::from_rgb(124, 220, 174), "SERVER RUNNING")
                        } else {
                            (Color32::from_rgb(220, 160, 94), "SERVER STOPPED")
                        };
                        ui.colored_label(dot, "●");
                        ui.label(RichText::new(text).size(12.0).strong().color(dot));
                    });
                });
            });

        egui::TopBottomPanel::bottom("footer")
            .frame(egui::Frame::default().inner_margin(egui::Margin::symmetric(26.0, 12.0)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.label(RichText::new(&self.status).color(muted()));
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.label(
                            RichText::new("TCP 9943  ·  UDP 9944")
                                .size(11.0)
                                .color(muted()),
                        );
                    });
                });
            });

        egui::CentralPanel::default()
            .frame(egui::Frame::default().inner_margin(egui::Margin::symmetric(26.0, 14.0)))
            .show(ctx, |ui| {
                ui.columns(2, |columns| {
                    self.settings_panel(&mut columns[0]);
                    self.logs_panel(&mut columns[1]);
                });
            });
    }
}

impl KitVrApp {
    fn settings_panel(&mut self, ui: &mut egui::Ui) {
        ui.heading(RichText::new("Stream setup").size(19.0));
        ui.add_space(10.0);
        egui::Frame::group(ui.style())
            .fill(Color32::from_rgb(22, 33, 35))
            .stroke(Stroke::new(1.0_f32, Color32::from_rgb(44, 61, 60)))
            .inner_margin(16.0)
            .show(ui, |ui| {
                let running = self.child.is_some();
                ui.add_enabled_ui(!running, |ui| {
                    field_label(ui, "VIDEO SOURCE");
                    egui::ComboBox::from_id_salt("source")
                        .selected_text(&self.source)
                        .width(ui.available_width())
                        .show_ui(ui, |ui| {
                            for source in ["Test pattern", "Desktop", "File"] {
                                ui.selectable_value(&mut self.source, source.to_string(), source);
                            }
                        });
                    if self.source == "File" {
                        ui.add_space(6.0);
                        ui.add(
                            egui::TextEdit::singleline(&mut self.file_path)
                                .hint_text("Path to video file")
                                .desired_width(ui.available_width()),
                        );
                    }

                    ui.add_space(14.0);
                    field_label(ui, "VIDEO ENCODER");
                    egui::ComboBox::from_id_salt("encoder")
                        .selected_text(&self.encoder)
                        .width(ui.available_width())
                        .show_ui(ui, |ui| {
                            for encoder in ENCODERS {
                                ui.selectable_value(
                                    &mut self.encoder,
                                    (*encoder).to_string(),
                                    *encoder,
                                );
                            }
                        });

                    ui.add_space(14.0);
                    ui.horizontal(|ui| {
                        ui.vertical(|ui| {
                            field_label(ui, "RESOLUTION");
                            egui::ComboBox::from_id_salt("resolution")
                                .selected_text(&self.resolution)
                                .width(145.0)
                                .show_ui(ui, |ui| {
                                    for resolution in RESOLUTIONS {
                                        ui.selectable_value(
                                            &mut self.resolution,
                                            (*resolution).to_string(),
                                            *resolution,
                                        );
                                    }
                                });
                        });
                        ui.vertical(|ui| {
                            field_label(ui, "FPS");
                            egui::ComboBox::from_id_salt("fps")
                                .selected_text(self.fps.to_string())
                                .width(80.0)
                                .show_ui(ui, |ui| {
                                    for fps in FRAME_RATES {
                                        ui.selectable_value(&mut self.fps, *fps, fps.to_string());
                                    }
                                });
                        });
                    });

                    ui.add_space(14.0);
                    field_label(ui, "BITRATE  ·  KBPS");
                    ui.add(
                        egui::Slider::new(&mut self.bitrate_kbps, 5_000..=100_000)
                            .step_by(5_000.0)
                            .suffix(" kbps"),
                    );

                    ui.add_space(14.0);
                    field_label(ui, "AUDIO");
                    ui.checkbox(&mut self.audio_enabled, "Send PC audio to headset");
                    if self.audio_enabled {
                        egui::ComboBox::from_id_salt("audio")
                            .selected_text(&self.audio_device)
                            .width(ui.available_width())
                            .show_ui(ui, |ui| {
                                ui.selectable_value(
                                    &mut self.audio_device,
                                    "default".to_string(),
                                    "System default output",
                                );
                                for device in &self.audio_devices {
                                    ui.selectable_value(
                                        &mut self.audio_device,
                                        device.clone(),
                                        device,
                                    );
                                }
                            });
                    }
                    ui.horizontal(|ui| {
                        ui.add(
                            egui::TextEdit::singleline(&mut self.microphone_device)
                                .hint_text("Optional PC mic destination")
                                .desired_width(ui.available_width() - 92.0),
                        );
                        if ui.small_button("List audio").clicked() {
                            self.refresh_audio_devices();
                        }
                    });
                });

                ui.add_space(16.0);
                if running {
                    if ui
                        .button(RichText::new("Stop streaming").strong())
                        .clicked()
                    {
                        self.stop_server();
                    }
                } else if ui
                    .button(RichText::new("Start streaming").strong())
                    .clicked()
                {
                    self.start_server();
                }
            });

        ui.add_space(16.0);
        egui::Frame::group(ui.style())
            .fill(Color32::from_rgb(22, 33, 35))
            .stroke(Stroke::new(1.0_f32, Color32::from_rgb(44, 61, 60)))
            .inner_margin(16.0)
            .show(ui, |ui| {
                field_label(ui, "QUEST CONNECTION ADDRESS");
                ui.horizontal(|ui| {
                    ui.label(
                        RichText::new(&self.host_ip)
                            .size(20.0)
                            .strong()
                            .color(Color32::WHITE),
                    );
                    if ui.small_button("Copy").clicked() {
                        ui.output_mut(|output| output.copied_text = self.host_ip.clone());
                    }
                    if ui.small_button("Refresh").clicked() {
                        self.host_ip = local_ip();
                    }
                });
                ui.label(
                    RichText::new("Enter this address in KitVR on your headset.").color(muted()),
                );
            });
    }

    fn logs_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading(RichText::new("Server output").size(19.0));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if ui.small_button("Clear").clicked() {
                    self.logs.clear();
                }
            });
        });
        ui.add_space(10.0);
        egui::Frame::group(ui.style())
            .fill(Color32::from_rgb(10, 16, 18))
            .stroke(Stroke::new(1.0_f32, Color32::from_rgb(44, 61, 60)))
            .inner_margin(12.0)
            .show(ui, |ui| {
                egui::ScrollArea::vertical()
                    .stick_to_bottom(true)
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        if self.logs.is_empty() {
                            ui.label(
                                RichText::new("Server messages will appear here.").color(muted()),
                            );
                        }
                        for line in &self.logs {
                            ui.label(
                                RichText::new(line)
                                    .monospace()
                                    .size(12.0)
                                    .color(Color32::from_rgb(183, 199, 191)),
                            );
                        }
                    });
            });
    }
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

fn field_label(ui: &mut egui::Ui, text: &str) {
    ui.label(
        RichText::new(text)
            .size(10.0)
            .strong()
            .color(Color32::from_rgb(139, 165, 155)),
    );
}

fn muted() -> Color32 {
    Color32::from_rgb(153, 170, 165)
}
