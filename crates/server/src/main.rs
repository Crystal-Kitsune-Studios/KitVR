mod audio;
mod bridge;
#[cfg(any(target_os = "linux", windows))]
mod mouse;
mod platform;
#[cfg(target_os = "linux")]
mod wayland;

use std::io::{self, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::sync_channel;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
#[cfg(any(target_os = "linux", windows))]
use std::time::Instant;

use kitvr_core::proto::audio::{pcm_from_payload, AudioHeader, PACKET_TYPE_AUDIO_UP};
use kitvr_core::proto::control::{decode_message, encode_message, Codec, Message};
use kitvr_core::proto::tracking::{
    BodyTracking, Tracking, ViewConfig, PACKET_TYPE_BODY_TRACKING, PACKET_TYPE_TRACKING,
    PACKET_TYPE_VIEW_CONFIG,
};
use kitvr_core::proto::{DecodeError, CONTROL_PORT, MEDIA_PORT, PROTOCOL_VERSION};
use mdns_sd::{ServiceDaemon, ServiceInfo};

const STEAMVR_TRACKING_PORT: u16 = 9945;

const USAGE: &str = "\
kitvr-server [options]

Video:
  --source testpattern|desktop|file:<path>   what to stream (default testpattern)
  --encoder libx265|hevc_nvenc|hevc_amf|hevc_qsv|hevc_vaapi   (default libx265)
    --width N --height N --fps N               encoded frame size and rate (default 1920 1080 60)
    --stereo-sbs                                file input is side-by-side; width is both eyes
  --bitrate-kbps N                           (default 40000)
  --gop N                                    frames between keyframes (default 60)

Audio:
  --list-audio                 print audio devices and exit
  --audio-device <name>        device to send to the headset, matched by name fragment
                               (default: system default output, captured in loopback)
  --mic-device <name>          device that receives the headset microphone, for example
                               a Voicemeeter virtual input (default: microphone off)
  --no-audio                   disable all audio

Input:
  --enable-mouse               allow the right hand/controller to control the PC mouse.
                               Only use on a trusted network: connections are not authenticated.

Requires ffmpeg on PATH, or set KITVR_FFMPEG to its full path.
Wayland desktop capture also requires a ScreenCast portal, PipeWire, and GStreamer
with gst-launch-1.0 and the pipewiresrc, videoconvert, videoscale, videorate, queue and fdsink plugins.
";

fn parse_number(option: &str, value: &str) -> Result<u32, String> {
    value.parse().map_err(|_| format!("{option} requires an unsigned integer"))
}

fn validate_settings(settings: &bridge::Settings) -> Result<(), &'static str> {
    if settings.width < 2 || settings.width > u16::MAX as u32 || settings.width % 2 != 0 {
        return Err("--width must be even and between 2 and 65534");
    }
    if settings.height < 2 || settings.height > u16::MAX as u32 || settings.height % 2 != 0 {
        return Err("--height must be even and between 2 and 65534");
    }
    if settings.fps == 0 || settings.fps > u16::MAX as u32 {
        return Err("--fps must be between 1 and 65535");
    }
    if settings.bitrate_kbps == 0 {
        return Err("--bitrate-kbps must be greater than zero");
    }
    if settings.gop == 0 {
        return Err("--gop must be greater than zero");
    }
    if settings.stereo_sbs && !matches!(&settings.source, bridge::Source::File(_)) {
        return Err("--stereo-sbs requires --source file:<path> with side-by-side video");
    }
    Ok(())
}

fn parse_args() -> (bridge::Settings, audio::AudioSettings, bool) {
    let mut s = bridge::Settings::default();
    let mut a = audio::AudioSettings::default();
    let mut mouse_enabled = false;
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
            "--width" | "--height" | "--fps" | "--bitrate-kbps" | "--gop" => {
                let value = parse_number(&arg, &next()).unwrap_or_else(|error| {
                    eprintln!("{error}\n{USAGE}");
                    std::process::exit(2);
                });
                match arg.as_str() {
                    "--width" => s.width = value,
                    "--height" => s.height = value,
                    "--fps" => s.fps = value,
                    "--bitrate-kbps" => s.bitrate_kbps = value,
                    "--gop" => s.gop = value,
                    _ => unreachable!(),
                }
            }
            "--stereo-sbs" => s.stereo_sbs = true,
            "--audio-device" => a.capture = Some(next()),
            "--mic-device" => a.mic_output = Some(next()),
            "--no-audio" => {
                a.capture = None;
                a.mic_output = None;
            }
            "--enable-mouse" => mouse_enabled = true,
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
    if let Err(error) = validate_settings(&s) {
        eprintln!("{error}\n{USAGE}");
        std::process::exit(2);
    }
    (s, a, mouse_enabled)
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

fn media_socket(control: &TcpStream, port: u16) -> io::Result<UdpSocket> {
    let local = SocketAddr::new(control.local_addr()?.ip(), port);
    let socket = UdpSocket::bind(local)?;
    socket.set_read_timeout(Some(Duration::from_millis(200)))?;
    Ok(socket)
}

fn handle_control(
    mut s: TcpStream,
    settings: &bridge::Settings,
    audio_cfg: &audio::AudioSettings,
    udp: &Arc<UdpSocket>,
    #[cfg(any(target_os = "linux", windows))] mouse: Option<Arc<mouse::Mouse>>,
) -> io::Result<()> {
    let peer_ip = s.peer_addr()?.ip();
    let stop = Arc::new(AtomicBool::new(false));
    let client: Arc<Mutex<Option<SocketAddr>>> = Arc::new(Mutex::new(None));
    let mut workers = Vec::new();
    let (mic_tx, mic_rx) = sync_channel::<Vec<i16>>(256);
    let mic_enabled = audio_cfg.mic_output.is_some();

    // Receives tracking and microphone packets, and learns where to send video.
    {
        let (udp, stop, client) = (udp.clone(), stop.clone(), client.clone());
        #[cfg(any(target_os = "linux", windows))]
        let mouse_backend = mouse;
        workers.push(thread::spawn(move || {
            let mut buf = [0u8; 2048];
            #[cfg(any(target_os = "linux", windows))]
            let mut mouse = mouse_backend.as_deref().map(mouse::Mouse::session);
            #[cfg(any(target_os = "linux", windows))]
            let mut last_tracking = None::<Instant>;
            while !stop.load(Ordering::Relaxed) {
                #[cfg(any(target_os = "linux", windows))]
                if last_tracking.is_some_and(|last| last.elapsed() > Duration::from_millis(350)) {
                    if let Some(input) = mouse.as_mut() {
                        if let Err(error) = input.reset() {
                            eprintln!("mouse control stopped: {error}");
                            mouse = None;
                        }
                    }
                    last_tracking = None;
                }
                let (n, from) = match udp.recv_from(&mut buf) {
                    Ok(packet) => packet,
                    Err(error) if matches!(error.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => continue,
                    Err(error) => {
                        eprintln!("media receive failed: {error}");
                        break;
                    }
                };
                if n == 0 {
                    continue;
                }
                if from.ip() != peer_ip {
                    continue;
                }
                let known_client = *client.lock().unwrap();
                if known_client.is_some_and(|address| address != from) {
                    continue;
                }
                if buf[0] == PACKET_TYPE_TRACKING {
                    if let Ok(t) = Tracking::parse(&buf[..n]) {
                        #[cfg(any(target_os = "linux", windows))]
                        {
                            if let Some(input) = mouse.as_mut() {
                                match input.update(&t) {
                                    Ok(true) => last_tracking = Some(Instant::now()),
                                    Ok(false) => {}
                                    Err(error) => {
                                        eprintln!("mouse control stopped: {error}");
                                        mouse = None;
                                    }
                                }
                            }
                        }
                        let _ = udp.send_to(&buf[..n], ("127.0.0.1", STEAMVR_TRACKING_PORT));
                        let mut c = client.lock().unwrap();
                        if *c != Some(from) {
                            println!("client media address: {from}");
                            *c = Some(from);
                        }
                    }
                } else if buf[0] == PACKET_TYPE_BODY_TRACKING {
                    if BodyTracking::parse(&buf[..n]).is_ok() {
                        let _ = udp.send_to(&buf[..n], ("127.0.0.1", STEAMVR_TRACKING_PORT));
                    }
                } else if buf[0] == PACKET_TYPE_VIEW_CONFIG {
                    if known_client == Some(from) && ViewConfig::parse(&buf[..n]).is_ok() {
                        let _ = udp.send_to(&buf[..n], ("127.0.0.1", STEAMVR_TRACKING_PORT));
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
        let control = s.try_clone()?;
        let (udp, stop, client) = (udp.clone(), stop.clone(), client.clone());
        let settings = settings.clone();
        workers.push(thread::spawn(move || {
            let Some(dest) = wait_client(&stop, &client) else { return };
            println!("streaming video to {dest}");
            if let Err(e) = bridge::run(&settings, &udp, dest, &stop) {
                eprintln!("video stopped: {e}");
                let _ = control.shutdown(Shutdown::Both);
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
                                stereo_sbs: settings.stereo_sbs,
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
    let (mut settings, audio_cfg, mouse_enabled) = parse_args();
    #[cfg(target_os = "linux")]
    if matches!(settings.source, bridge::Source::Desktop) && bridge::is_wayland_session() {
        settings.source = bridge::Source::Wayland(Arc::new(wayland::WaylandCapture::new()?));
    }
    #[cfg(any(target_os = "linux", windows))]
    let mouse = if mouse_enabled {
        eprintln!("Warning: mouse control is enabled; KitVR connections are not authenticated. Use only a trusted network.");
        Some(Arc::new(mouse::Mouse::new()?))
    } else {
        None
    };
    #[cfg(not(any(target_os = "linux", windows)))]
    if mouse_enabled {
        return Err(io::Error::other("mouse control is unsupported on this platform"));
    }
    let name = host_name();
    let _discovery = publish_discovery(&name);
    println!("kitvr-server, platform: {}", platform::NAME);
    println!("native capture:  {:?}", platform::create_capture().err().map(|e| e.to_string()));
    println!("native encoder:  {:?}", platform::create_encoder().err().map(|e| e.to_string()));
    println!("native input:    {:?}", platform::create_input().err().map(|e| e.to_string()));
    println!("using the ffmpeg bridge: {settings:?}");
    println!("audio: {audio_cfg:?}");

    let listener = TcpListener::bind(("0.0.0.0", CONTROL_PORT))?;
    println!("listening: control TCP {CONTROL_PORT}, media UDP {MEDIA_PORT} per session");

    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if let Ok(peer) = s.peer_addr() {
                    println!("control connection from {peer}");
                }
                let _ = s.set_nodelay(true);
                let result = media_socket(&s, MEDIA_PORT).and_then(|socket| {
                    #[cfg(any(target_os = "linux", windows))]
                    { handle_control(s, &settings, &audio_cfg, &Arc::new(socket), mouse.clone()) }
                    #[cfg(not(any(target_os = "linux", windows)))]
                    { handle_control(s, &settings, &audio_cfg, &Arc::new(socket)) }
                });
                if let Err(e) = result {
                    eprintln!("connection error: {e}");
                }
            }
            Err(e) => eprintln!("accept error: {e}"),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stream_settings_must_fit_the_wire_format_and_encoder() {
        let valid = bridge::Settings::default();
        assert!(validate_settings(&valid).is_ok());
        for width in [0, 1, 3, 65535, 65536] {
            assert!(validate_settings(&bridge::Settings { width, ..valid.clone() }).is_err());
        }
        for height in [0, 1, 3, 65535, 65536] {
            assert!(validate_settings(&bridge::Settings { height, ..valid.clone() }).is_err());
        }
        for fps in [0, 65536] {
            assert!(validate_settings(&bridge::Settings { fps, ..valid.clone() }).is_err());
        }
        assert!(validate_settings(&bridge::Settings { bitrate_kbps: 0, ..valid.clone() }).is_err());
        assert!(validate_settings(&bridge::Settings { gop: 0, ..valid.clone() }).is_err());
        assert!(validate_settings(&bridge::Settings {
            width: 65534, height: 65534, fps: 65535, ..valid.clone()
        }).is_ok());
        assert!(validate_settings(&bridge::Settings { stereo_sbs: true, ..valid.clone() }).is_err());
        assert!(validate_settings(&bridge::Settings {
            source: bridge::Source::File("stereo.mp4".into()), stereo_sbs: true, ..valid
        }).is_ok());
    }

    #[test]
    fn malformed_numeric_options_are_errors() {
        assert_eq!(parse_number("--fps", "90"), Ok(90));
        for value in ["", "-1", "abc", "4294967296"] {
            assert!(parse_number("--fps", value).is_err());
        }
    }

    #[test]
    fn media_uses_the_control_connections_local_address() -> io::Result<()> {
        let listener = TcpListener::bind(("127.0.0.1", 0))?;
        let _client = TcpStream::connect(listener.local_addr()?)?;
        let (control, _) = listener.accept()?;
        let media = media_socket(&control, 0)?;
        assert_eq!(media.local_addr()?.ip(), control.local_addr()?.ip());

        let receiver = UdpSocket::bind(("127.0.0.1", 0))?;
        receiver.set_read_timeout(Some(Duration::from_secs(2)))?;
        media.send_to(b"video", receiver.local_addr()?)?;
        let mut buf = [0; 8];
        let (len, from) = receiver.recv_from(&mut buf)?;
        assert_eq!(&buf[..len], b"video");
        assert_eq!(from, media.local_addr()?);
        Ok(())
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn media_source_matches_the_selected_address_on_a_multihomed_host() -> io::Result<()> {
        let listener = TcpListener::bind(("127.0.0.2", 0))?;
        let _client = TcpStream::connect(listener.local_addr()?)?;
        let (control, _) = listener.accept()?;
        let media = media_socket(&control, 0)?;
        let receiver = UdpSocket::bind(("127.0.0.1", 0))?;
        receiver.set_read_timeout(Some(Duration::from_secs(2)))?;
        media.send_to(b"video", receiver.local_addr()?)?;
        let mut buf = [0; 8];
        let (_, from) = receiver.recv_from(&mut buf)?;
        assert_eq!(from.ip(), control.local_addr()?.ip());
        Ok(())
    }
}
