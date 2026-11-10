mod audio;
mod bridge;
mod platform;

use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::sync_channel;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use kitvr_core::proto::audio::{pcm_from_payload, AudioHeader, PACKET_TYPE_AUDIO_UP};
use kitvr_core::proto::control::{decode_message, encode_message, Codec, Message};
use kitvr_core::proto::tracking::{Tracking, PACKET_TYPE_TRACKING};
use kitvr_core::proto::{DecodeError, CONTROL_PORT, MEDIA_PORT, PROTOCOL_VERSION};
use mdns_sd::{ServiceDaemon, ServiceInfo};

const STEAMVR_TRACKING_PORT: u16 = 9945;

const USAGE: &str = "\
kitvr-server [options]

Video:
  --source testpattern|desktop|file:<path>   what to stream (default testpattern)
  --encoder libx265|hevc_nvenc|hevc_amf|hevc_qsv|hevc_vaapi   (default libx265)
  --width N --height N --fps N               (default 1920 1080 60)
  --bitrate-kbps N                           (default 40000)
  --gop N                                    frames between keyframes (default 60)

Audio:
  --list-audio                 print audio devices and exit
  --audio-device <name>        device to send to the headset, matched by name fragment
                               (default: system default output, captured in loopback)
  --mic-device <name>          device that receives the headset microphone, for example
                               a Voicemeeter virtual input (default: microphone off)
  --no-audio                   disable all audio

Requires ffmpeg on PATH, or set KITVR_FFMPEG to its full path.
";

fn parse_args() -> (bridge::Settings, audio::AudioSettings) {
    let mut s = bridge::Settings::default();
    let mut a = audio::AudioSettings::default();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut next = || args.next().unwrap_or_default();
        match arg.as_str() {
            "--source" => {
                let v = next();
                s.source = match v.as_str() {
                    "testpattern" => bridge::Source::TestPattern,
                    "desktop" => bridge::Source::Desktop,
                    other => match other.strip_prefix("file:") {
                        Some(p) => bridge::Source::File(p.to_string()),
                        None => {
                            eprintln!("unknown source '{other}'\n{USAGE}");
                            std::process::exit(2);
                        }
                    },
                };
            }
            "--encoder" => s.encoder = next(),
            "--width" => s.width = next().parse().unwrap_or(s.width),
            "--height" => s.height = next().parse().unwrap_or(s.height),
            "--fps" => s.fps = next().parse().unwrap_or(s.fps),
            "--bitrate-kbps" => s.bitrate_kbps = next().parse().unwrap_or(s.bitrate_kbps),
            "--gop" => s.gop = next().parse().unwrap_or(s.gop),
            "--audio-device" => a.capture = Some(next()),
            "--mic-device" => a.mic_output = Some(next()),
            "--no-audio" => {
                a.capture = None;
                a.mic_output = None;
            }
            "--list-audio" => {
                for line in audio::list_devices() {
                    println!("{line}");
                }
                std::process::exit(0);
            }
            "--help" | "-h" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            other => {
                eprintln!("unknown option '{other}'\n{USAGE}");
                std::process::exit(2);
            }
        }
    }
    (s, a)
}

fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "kitvr-server".to_string())
}

fn publish_discovery(name: &str) -> Option<ServiceDaemon> {
    let daemon = match ServiceDaemon::new() {
        Ok(daemon) => daemon,
        Err(error) => {
            eprintln!("mDNS unavailable; use the PC's IP address: {error}");
            return None;
        }
    };
    let host_label: String = name
        .chars()
        .filter(|character| character.is_ascii_alphanumeric() || *character == '-')
        .flat_map(char::to_lowercase)
        .take(48)
        .collect();
    let host_label = if host_label.is_empty() { "kitvr" } else { &host_label };
    let hostname = format!("kitvr-{host_label}.local.");
    let instance = format!("KitVR on {name}");
    let properties = [("ver", "1"), ("name", name)];
    let service = match ServiceInfo::new(
        "_kitvr._tcp.local.",
        &instance,
        &hostname,
        "",
        CONTROL_PORT,
        &properties[..],
    ) {
        Ok(service) => service.enable_addr_auto(),
        Err(error) => {
            eprintln!("could not create mDNS service; use the PC's IP address: {error}");
            return None;
        }
    };
    if let Err(error) = daemon.register(service) {
        eprintln!("could not advertise mDNS service; use the PC's IP address: {error}");
        return None;
    }
    println!("discovery: _kitvr._tcp.local.");
    Some(daemon)
}

/// Blocks until the first tracking packet reveals the client's UDP address.
fn wait_client(stop: &AtomicBool, client: &Mutex<Option<SocketAddr>>) -> Option<SocketAddr> {
    loop {
        if stop.load(Ordering::Relaxed) {
            return None;
        }
        if let Some(a) = *client.lock().unwrap() {
            return Some(a);
        }
        thread::sleep(Duration::from_millis(20));
    }
}

fn handle_control(
    mut s: TcpStream,
    settings: &bridge::Settings,
    audio_cfg: &audio::AudioSettings,
    udp: &Arc<UdpSocket>,
) -> io::Result<()> {
    let stop = Arc::new(AtomicBool::new(false));
    let client: Arc<Mutex<Option<SocketAddr>>> = Arc::new(Mutex::new(None));
    let pose_ts = Arc::new(AtomicU64::new(0));
    let mut workers = Vec::new();
    let (mic_tx, mic_rx) = sync_channel::<Vec<i16>>(256);
    let mic_enabled = audio_cfg.mic_output.is_some();

    // Receives tracking and microphone packets, and learns where to send video.
    {
        let (udp, stop, client, pose_ts) = (udp.clone(), stop.clone(), client.clone(), pose_ts.clone());
        workers.push(thread::spawn(move || {
            let mut buf = [0u8; 2048];
            while !stop.load(Ordering::Relaxed) {
                let Ok((n, from)) = udp.recv_from(&mut buf) else { continue };
                if n == 0 {
                    continue;
                }
                if buf[0] == PACKET_TYPE_TRACKING {
                    if let Ok(t) = Tracking::parse(&buf[..n]) {
                        pose_ts.store(t.timestamp_us, Ordering::Relaxed);
                        let _ = udp.send_to(&buf[..n], ("127.0.0.1", STEAMVR_TRACKING_PORT));
                        let mut c = client.lock().unwrap();
                        if *c != Some(from) {
                            println!("client media address: {from}");
                            *c = Some(from);
                        }
                    }
                } else if buf[0] == PACKET_TYPE_AUDIO_UP && mic_enabled {
                    if let Ok((_, payload)) = AudioHeader::parse(&buf[..n]) {
                        let _ = mic_tx.try_send(pcm_from_payload(payload));
                    }
                }
            }
        }));
    }

    // Video.
    {
        let (udp, stop, client, pose_ts) = (udp.clone(), stop.clone(), client.clone(), pose_ts.clone());
        let settings = settings.clone();
        workers.push(thread::spawn(move || {
            let Some(dest) = wait_client(&stop, &client) else { return };
            println!("streaming video to {dest}");
            if let Err(e) = bridge::run(&settings, &udp, dest, &stop, &pose_ts) {
                eprintln!("video stopped: {e}");
            }
        }));
    }

    // Audio to the headset.
    if let Some(device) = audio_cfg.capture.clone() {
        let (udp, stop, client) = (udp.clone(), stop.clone(), client.clone());
        workers.push(thread::spawn(move || {
            let Some(dest) = wait_client(&stop, &client) else { return };
            if let Err(e) = audio::run_downstream(&device, &udp, dest, &stop) {
                eprintln!("audio to headset stopped: {e}");
            }
        }));
    }

    // Microphone from the headset.
    if let Some(device) = audio_cfg.mic_output.clone() {
        let stop = stop.clone();
        workers.push(thread::spawn(move || {
            if let Err(e) = audio::run_upstream(&device, &stop, mic_rx) {
                eprintln!("microphone output stopped: {e}");
            }
        }));
    }

    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 1024];
    let result = 'conn: loop {
        let n = match s.read(&mut tmp) {
            Ok(0) => break Ok(()),
            Ok(n) => n,
            Err(e) => break Err(e),
        };
        buf.extend_from_slice(&tmp[..n]);
        loop {
            match decode_message(&buf) {
                Ok((msg, used)) => {
                    buf.drain(..used);
                    match msg {
                        Message::Hello { version, name } => {
                            println!("client hello: {name} (protocol {version})");
                            if version != PROTOCOL_VERSION {
                                let _ = s.write_all(&encode_message(&Message::Bye));
                                break 'conn Ok(());
                            }
                            let mut out = encode_message(&Message::Hello {
                                version: PROTOCOL_VERSION,
                                name: host_name(),
                            });
                            out.extend_from_slice(&encode_message(&Message::StreamConfig {
                                codec: Codec::Hevc,
                                width: settings.width as u16,
                                height: settings.height as u16,
                                fps: settings.fps as u16,
                                bitrate_kbps: settings.bitrate_kbps,
                            }));
                            if audio_cfg.capture.is_some() || audio_cfg.mic_output.is_some() {
                                out.extend_from_slice(&encode_message(&Message::AudioConfig {
                                    sample_rate: audio::SAMPLE_RATE,
                                    channels: if audio_cfg.capture.is_some() { audio::CHANNELS } else { 0 },
                                    frame_samples: audio::FRAME_SAMPLES,
                                    mic_channels: if mic_enabled { audio::MIC_CHANNELS } else { 0 },
                                }));
                            }
                            if let Err(e) = s.write_all(&out) {
                                break 'conn Err(e);
                            }
                        }
                        Message::Ping { ts_us } => {
                            if let Err(e) = s.write_all(&encode_message(&Message::Pong { ts_us })) {
                                break 'conn Err(e);
                            }
                        }
                        Message::KeyframeRequest { last_good_frame } => {
                            println!("keyframe requested after frame {last_good_frame} (ffmpeg bridge uses a fixed GOP)");
                        }
                        Message::BitrateRequest { bitrate_kbps } => {
                            println!("bitrate request {bitrate_kbps} kbps ignored by the ffmpeg bridge");
                        }
                        Message::Bye => break 'conn Ok(()),
                        _ => {}
                    }
                }
                Err(DecodeError::Truncated) => break,
                Err(e) => {
                    eprintln!("protocol error: {e}");
                    break 'conn Ok(());
                }
            }
        }
    };

    stop.store(true, Ordering::Relaxed);
    for w in workers {
        let _ = w.join();
    }
    println!("session ended");
    result
}

fn main() -> io::Result<()> {
    let (settings, audio_cfg) = parse_args();
    let name = host_name();
    let _discovery = publish_discovery(&name);
    println!("kitvr-server, platform: {}", platform::NAME);
    println!("native capture:  {:?}", platform::create_capture().err().map(|e| e.to_string()));
    println!("native encoder:  {:?}", platform::create_encoder().err().map(|e| e.to_string()));
    println!("native input:    {:?}", platform::create_input().err().map(|e| e.to_string()));
    println!("using the ffmpeg bridge: {settings:?}");
    println!("audio: {audio_cfg:?}");

    let udp = Arc::new(UdpSocket::bind(("0.0.0.0", MEDIA_PORT))?);
    udp.set_read_timeout(Some(Duration::from_millis(200)))?;
    let listener = TcpListener::bind(("0.0.0.0", CONTROL_PORT))?;
    println!("listening: control TCP {CONTROL_PORT}, media UDP {MEDIA_PORT}");

    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if let Ok(peer) = s.peer_addr() {
                    println!("control connection from {peer}");
                }
                let _ = s.set_nodelay(true);
                if let Err(e) = handle_control(s, &settings, &audio_cfg, &udp) {
                    eprintln!("connection error: {e}");
                }
            }
            Err(e) => eprintln!("accept error: {e}"),
        }
    }
    Ok(())
}
