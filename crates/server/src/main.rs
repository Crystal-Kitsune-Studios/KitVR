mod audio;
mod bridge;
#[cfg(any(target_os = "linux", windows))]
mod mouse;
mod platform;
#[cfg(any(target_os = "linux", windows))]
mod steamvr;
#[cfg(target_os = "linux")]
mod wayland;

use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::sync_channel;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use kitvr_core::proto::audio::{pcm_from_payload, AudioHeader, PACKET_TYPE_AUDIO_UP};
use kitvr_core::proto::control::{decode_message, encode_message, Codec, Message};
use kitvr_core::proto::crypto::{self, SessionKeys};
use kitvr_core::proto::tracking::{
    BodyTracking, Tracking, ViewConfig, PACKET_TYPE_BODY_TRACKING, PACKET_TYPE_TRACKING,
    PACKET_TYPE_VIEW_CONFIG,
};
use kitvr_core::proto::{DecodeError, AVATAR_TRACKING_PORT, CONTROL_PORT, MEDIA_PORT, PROTOCOL_VERSION};
use mdns_sd::{ServiceDaemon, ServiceInfo};

const STEAMVR_TRACKING_PORT: u16 = 9945;
const CONTROL_IDLE_TIMEOUT: Duration = Duration::from_secs(15);

const USAGE: &str = "\
kitvr-server [options]

Video:
  --source testpattern|desktop|file:<path>|steamvr
                                             what to stream (default testpattern)
                                             steamvr reads SteamVR's frames: from the KitVR
                                             Vulkan layer on Linux, from the KitVR driver on Windows
  --encoder libx265|hevc_nvenc|hevc_amf|hevc_qsv|hevc_vaapi   (default libx265)
    --width N --height N --fps N               encoded frame size and rate (default 1920 1080 60)
    --stereo-sbs                                file input is side-by-side; width is both eyes
  --bitrate-kbps N                           (default 20000)
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
                               Requires a paired, authenticated connection.
  --avatar-tracking            forward tracking to a local desktop avatar on UDP loopback.

Security:
  --show-pairing-key            print the existing Quest pairing key and exit

Requires ffmpeg on PATH, or set KITVR_FFMPEG to its full path.
Wayland desktop capture also requires a ScreenCast portal, PipeWire, and GStreamer
with gst-launch-1.0 and the pipewiresrc, videoconvert, videoscale, videorate, queue and fdsink plugins.
";

fn parse_number(option: &str, value: &str) -> Result<u32, String> {
    value.parse().map_err(|_| format!("{option} requires an unsigned integer"))
}

fn stereo_capable(source: &bridge::Source) -> bool {
    match source {
        bridge::Source::File(_) => true,
        #[cfg(any(target_os = "linux", windows))]
        bridge::Source::SteamVr(_) => true,
        _ => false,
    }
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
    if settings.stereo_sbs && !stereo_capable(&settings.source) {
        return Err("--stereo-sbs requires --source file:<path> or --source steamvr");
    }
    Ok(())
}

fn parse_args() -> (bridge::Settings, audio::AudioSettings, bool, bool) {
    let mut s = bridge::Settings::default();
    let mut a = audio::AudioSettings::default();
    let mut mouse_enabled = false;
    let mut avatar_tracking = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut next = || args.next().unwrap_or_default();
        match arg.as_str() {
            "--source" => {
                let v = next();
                s.source = match v.as_str() {
                    "testpattern" => bridge::Source::TestPattern,
                    "desktop" => bridge::Source::Desktop,
                    "steamvr" => {
                        #[cfg(any(target_os = "linux", windows))]
                        {
                            let name = std::env::var("KITVR_STEAMVR_SHM")
                                .unwrap_or_else(|_| steamvr::DEFAULT_NAME.to_string());
                            match steamvr::SteamVrRing::open(&name) {
                                Ok(ring) => bridge::Source::SteamVr(ring),
                                Err(error) => {
                                    eprintln!("{error}");
                                    std::process::exit(2);
                                }
                            }
                        }
                        #[cfg(not(any(target_os = "linux", windows)))]
                        {
                            eprintln!("--source steamvr is only available on Linux and Windows");
                            std::process::exit(2);
                        }
                    }
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
            "--avatar-tracking" => avatar_tracking = true,
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
    (s, a, mouse_enabled, avatar_tracking)
}

fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "kitvr-server".to_string())
}

fn pairing_key_path() -> io::Result<PathBuf> {
    if let Some(path) = std::env::var_os("KITVR_PAIRING_KEY_FILE") {
        return Ok(PathBuf::from(path));
    }
    #[cfg(windows)]
    let root = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("USERPROFILE")
                .map(|home| PathBuf::from(home).join("AppData").join("Roaming"))
        })
        .ok_or_else(|| io::Error::other("APPDATA or USERPROFILE is required for pairing key storage"))?;
    #[cfg(not(windows))]
    let root = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .ok_or_else(|| io::Error::other("XDG_CONFIG_HOME or HOME is required for pairing key storage"))?;
    Ok(root.join("kitvr").join("pairing.key"))
}

fn decode_pairing_key(contents: &str) -> io::Result<[u8; 32]> {
    let hex = contents.trim();
    if hex.len() != 64 || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "pairing key file must contain exactly 64 hexadecimal characters",
        ));
    }
    let mut key = [0; 32];
    for (index, pair) in hex.as_bytes().chunks_exact(2).enumerate() {
        let digit = |byte: u8| match byte {
            b'0'..=b'9' => byte - b'0',
            b'a'..=b'f' => byte - b'a' + 10,
            b'A'..=b'F' => byte - b'A' + 10,
            _ => unreachable!(),
        };
        key[index] = digit(pair[0]) << 4 | digit(pair[1]);
    }
    Ok(key)
}

fn encode_pairing_key(key: &[u8; 32]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(64);
    for byte in key {
        encoded.push(HEX[(byte >> 4) as usize] as char);
        encoded.push(HEX[(byte & 0x0f) as usize] as char);
    }
    encoded
}

fn read_pairing_key() -> io::Result<[u8; 32]> {
    let path = pairing_key_path()?;
    let contents = fs::read_to_string(&path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(&path)?.permissions().mode();
        if mode & 0o077 != 0 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                format!("pairing key file {} must be accessible only to its owner", path.display()),
            ));
        }
    }
    decode_pairing_key(&contents)
}

fn load_or_create_pairing_key() -> io::Result<[u8; 32]> {
    let path = pairing_key_path()?;
    match read_pairing_key() {
        Ok(key) => return Ok(key),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }

    let parent = path.parent().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "pairing key path has no parent directory")
    })?;
    fs::create_dir_all(parent)?;
    let mut key = [0; 32];
    crypto::random_bytes(&mut key).map_err(|error| io::Error::other(error.to_string()))?;
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    match options.open(&path) {
        Ok(mut file) => {
            writeln!(file, "{}", encode_pairing_key(&key))?;
            file.sync_all()?;
            println!(
                "pairing key (copy into the Quest app; keep it private): {}",
                encode_pairing_key(&key)
            );
            Ok(key)
        }
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
            read_pairing_key()
        }
        Err(error) => Err(error),
    }
}

fn authenticate_control(stream: &mut TcpStream, pairing_key: &[u8; 32]) -> io::Result<SessionKeys> {
    const HANDSHAKE_LEN: usize = 68;
    let mut request = [0; HANDSHAKE_LEN];
    stream.read_exact(&mut request)?;
    if &request[..4] != b"KVH2" {
        return Err(io::Error::new(io::ErrorKind::PermissionDenied, "secure handshake required"));
    }
    let client_nonce: [u8; 32] = request[4..36].try_into().unwrap();
    crypto::verify_client_authenticator(pairing_key, &client_nonce, &request[36..])
        .map_err(|error| io::Error::new(io::ErrorKind::PermissionDenied, error))?;

    let mut server_nonce = [0; 32];
    crypto::random_bytes(&mut server_nonce)
        .map_err(|error| io::Error::other(error.to_string()))?;
    let proof = crypto::server_authenticator(pairing_key, &client_nonce, &server_nonce);
    let mut response = Vec::with_capacity(HANDSHAKE_LEN);
    response.extend_from_slice(b"KVS2");
    response.extend_from_slice(&server_nonce);
    response.extend_from_slice(&proof);
    stream.write_all(&response)?;
    crypto::derive_session_keys(pairing_key, &client_nonce, &server_nonce)
        .map_err(|error| io::Error::other(error.to_string()))
}

struct SecureControlReader {
    buf: Vec<u8>,
    expected_sequence: u64,
}

impl SecureControlReader {
    fn new() -> Self {
        Self { buf: Vec::new(), expected_sequence: 0 }
    }

    fn feed(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    fn next(&mut self, key: &[u8; 32]) -> io::Result<Option<Vec<u8>>> {
        if self.buf.len() < 4 {
            return Ok(None);
        }
        let length = u32::from_le_bytes(self.buf[..4].try_into().unwrap()) as usize;
        if !(24..=64 * 1024 + 24).contains(&length) {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid encrypted control record length"));
        }
        if self.buf.len() < 4 + length {
            return Ok(None);
        }
        let sequence = u64::from_le_bytes(self.buf[4..12].try_into().unwrap());
        if sequence != self.expected_sequence {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "encrypted control sequence mismatch"));
        }
        let mut aad = b"KitVR v2 control".to_vec();
        aad.extend_from_slice(&sequence.to_le_bytes());
        let plaintext = crypto::open(key, sequence, &aad, &self.buf[12..4 + length])
            .map_err(|error| io::Error::new(io::ErrorKind::PermissionDenied, error))?;
        self.buf.drain(..4 + length);
        self.expected_sequence = self.expected_sequence.checked_add(1)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "encrypted control sequence exhausted"))?;
        Ok(Some(plaintext))
    }
}

fn write_secure_control(
    stream: &mut TcpStream,
    key: &[u8; 32],
    sequence: &mut u64,
    plaintext: &[u8],
) -> io::Result<()> {
    let record = crypto::seal_control_record(key, *sequence, plaintext)
        .map_err(|error| io::Error::other(error.to_string()))?;
    stream.write_all(&record)?;
    *sequence = sequence
        .checked_add(1)
        .ok_or_else(|| io::Error::other("encrypted control sequence exhausted"))?;
    Ok(())
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
    let properties = [("ver", "2"), ("name", name)];
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

fn accepts_media_sender(
    packet_type: u8,
    from: SocketAddr,
    peer_ip: std::net::IpAddr,
    known_client: Option<SocketAddr>,
    handshake_complete: bool,
) -> bool {
    if !handshake_complete || from.ip() != peer_ip {
        return false;
    }
    match known_client {
        Some(client) => from == client,
        None => packet_type == PACKET_TYPE_TRACKING,
    }
}

fn handle_control(
    mut s: TcpStream,
    pairing_key: &[u8; 32],
    settings: &bridge::Settings,
    audio_cfg: &audio::AudioSettings,
    udp: &Arc<UdpSocket>,
    avatar_socket: Option<Arc<UdpSocket>>,
    #[cfg(any(target_os = "linux", windows))] mouse: Option<Arc<mouse::Mouse>>,
    idle_timeout: Duration,
) -> io::Result<()> {
    let peer_ip = s.peer_addr()?.ip();
    s.set_read_timeout(Some(Duration::from_secs(5)))?;
    let keys = authenticate_control(&mut s, pairing_key)?;
    s.set_read_timeout(Some(idle_timeout.min(Duration::from_millis(200))))?;
    let video_control = s.try_clone()?;
    let stop = Arc::new(AtomicBool::new(false));
    let handshake_complete = Arc::new(AtomicBool::new(true));
    let client: Arc<Mutex<Option<SocketAddr>>> = Arc::new(Mutex::new(None));
    let media_sender = Arc::new(bridge::SecureMediaSender::new(
        udp.clone(),
        keys.server_to_client_media,
    ));
    let mut workers = Vec::new();
    let (mic_tx, mic_rx) = sync_channel::<Vec<i16>>(256);
    let mic_enabled = audio_cfg.mic_output.is_some();

    // Receives tracking and microphone packets, and learns where to send video.
    {
        let (udp, stop, client) = (udp.clone(), stop.clone(), client.clone());
        let handshake_complete = handshake_complete.clone();
        let avatar_socket = avatar_socket.clone();
        let media_key = keys.client_to_server_media;
        #[cfg(any(target_os = "linux", windows))]
        let mouse_backend = mouse;
        workers.push(thread::spawn(move || {
            let mut buf = [0u8; 4096];
            let mut replay = crypto::ReplayWindow::default();
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
                let Ok((_, packet)) = crypto::open_media_datagram(&media_key, &mut replay, &buf[..n]) else {
                    continue;
                };
                if packet.is_empty() {
                    continue;
                }
                let known_client = *client.lock().unwrap();
                if !accepts_media_sender(
                    packet[0], from, peer_ip, known_client, handshake_complete.load(Ordering::Acquire),
                ) {
                    continue;
                }
                if packet[0] == PACKET_TYPE_TRACKING {
                    if let Ok(t) = Tracking::parse(&packet) {
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
                        let _ = udp.send_to(&packet, ("127.0.0.1", STEAMVR_TRACKING_PORT));
                        if let Some(socket) = avatar_socket.as_ref() {
                            if let Err(error) = socket.send_to(&packet, ("127.0.0.1", AVATAR_TRACKING_PORT)) {
                                eprintln!("avatar tracking forward failed: {error}");
                            }
                        }
                        let mut c = client.lock().unwrap();
                        if *c != Some(from) {
                            println!("client media address: {from}");
                            *c = Some(from);
                        }
                    }
                } else if packet[0] == PACKET_TYPE_BODY_TRACKING {
                    if BodyTracking::parse(&packet).is_ok() {
                        let _ = udp.send_to(&packet, ("127.0.0.1", STEAMVR_TRACKING_PORT));
                        if known_client == Some(from) {
                            if let Some(socket) = avatar_socket.as_ref() {
                                if let Err(error) = socket.send_to(&packet, ("127.0.0.1", AVATAR_TRACKING_PORT)) {
                                    eprintln!("avatar body tracking forward failed: {error}");
                                }
                            }
                        }
                    }
                } else if packet[0] == PACKET_TYPE_VIEW_CONFIG {
                    if known_client == Some(from) && ViewConfig::parse(&packet).is_ok() {
                        let _ = udp.send_to(&packet, ("127.0.0.1", STEAMVR_TRACKING_PORT));
                    }
                } else if packet[0] == PACKET_TYPE_AUDIO_UP && mic_enabled {
                    if let Ok((_, payload)) = AudioHeader::parse(&packet) {
                        let _ = mic_tx.try_send(pcm_from_payload(payload));
                    }
                }
            }
        }));
    }

    // Video.
    {
        let control = video_control;
        let (sender, stop, client) = (media_sender.clone(), stop.clone(), client.clone());
        let settings = settings.clone();
        workers.push(thread::spawn(move || {
            let Some(dest) = wait_client(&stop, &client) else { return };
            println!("streaming video to {dest}");
            if let Err(e) = bridge::run(&settings, &*sender, dest, &stop) {
                eprintln!("video stopped: {e}");
                let _ = control.shutdown(Shutdown::Both);
            }
        }));
    }

    // Audio to the headset.
    if let Some(device) = audio_cfg.capture.clone() {
        let (sender, stop, client) = (media_sender.clone(), stop.clone(), client.clone());
        workers.push(thread::spawn(move || {
            let Some(dest) = wait_client(&stop, &client) else { return };
            if let Err(e) = audio::run_downstream(&device, &*sender, dest, &stop) {
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
    let mut last_message = Instant::now();
    let mut encrypted_reader = SecureControlReader::new();
    let mut server_sequence = 0;
    let result = 'conn: loop {
        if last_message.elapsed() >= idle_timeout {
            break Err(io::Error::new(io::ErrorKind::TimedOut, "control connection timed out"));
        }
        let n = match s.read(&mut tmp) {
            Ok(0) => break Ok(()),
            Ok(n) => n,
            Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted) => continue,
            Err(e) => break Err(e),
        };
        encrypted_reader.feed(&tmp[..n]);
        loop {
            match encrypted_reader.next(&keys.client_to_server_control) {
                Ok(Some(plaintext)) => buf.extend_from_slice(&plaintext),
                Ok(None) => break,
                Err(error) => break 'conn Err(error),
            }
        }
        while !buf.is_empty() {
            match decode_message(&buf) {
                Ok((msg, used)) => {
                    last_message = Instant::now();
                    buf.drain(..used);
                    match msg {
                        Message::Hello { version, name } => {
                            println!("client hello: {name} (protocol {version})");
                            if version != PROTOCOL_VERSION {
                                let _ = write_secure_control(
                                    &mut s, &keys.server_to_client_control,
                                    &mut server_sequence, &encode_message(&Message::Bye),
                                );
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
                            if let Err(e) = write_secure_control(
                                &mut s, &keys.server_to_client_control,
                                &mut server_sequence, &out,
                            ) {
                                break 'conn Err(e);
                            }
                        }
                        Message::Ping { ts_us } => {
                            if let Err(e) = write_secure_control(
                                &mut s, &keys.server_to_client_control, &mut server_sequence,
                                &encode_message(&Message::Pong { ts_us }),
                            ) {
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
    if std::env::args().any(|arg| arg == "--show-pairing-key") {
        let key = read_pairing_key()?;
        println!("pairing key (keep it private): {}", encode_pairing_key(&key));
        return Ok(());
    }
    let (mut settings, audio_cfg, mouse_enabled, avatar_tracking) = parse_args();
    let avatar_socket = if avatar_tracking {
        eprintln!("Warning: avatar tracking is forwarded to local processes.");
        Some(Arc::new(UdpSocket::bind(("127.0.0.1", 0))?))
    } else {
        None
    };
    #[cfg(target_os = "linux")]
    if matches!(settings.source, bridge::Source::Desktop) && bridge::is_wayland_session() {
        settings.source = bridge::Source::Wayland(Arc::new(wayland::WaylandCapture::new()?));
    }
    #[cfg(any(target_os = "linux", windows))]
    let mouse = if mouse_enabled {
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
    let pairing_key = load_or_create_pairing_key()?;

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
                    { handle_control(s, &pairing_key, &settings, &audio_cfg, &Arc::new(socket), avatar_socket.clone(), mouse.clone(), CONTROL_IDLE_TIMEOUT) }
                    #[cfg(not(any(target_os = "linux", windows)))]
                    { handle_control(s, &pairing_key, &settings, &audio_cfg, &Arc::new(socket), avatar_socket.clone(), CONTROL_IDLE_TIMEOUT) }
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

    fn authenticate_test_client(stream: &mut TcpStream, key: &[u8; 32]) -> SessionKeys {
        let client_nonce = [3; 32];
        let proof = crypto::client_authenticator(key, &client_nonce);
        let mut request = Vec::with_capacity(68);
        request.extend_from_slice(b"KVH2");
        request.extend_from_slice(&client_nonce);
        request.extend_from_slice(&proof);
        stream.write_all(&request).unwrap();
        let mut response = [0; 68];
        stream.read_exact(&mut response).unwrap();
        assert_eq!(&response[..4], b"KVS2");
        let server_nonce: [u8; 32] = response[4..36].try_into().unwrap();
        crypto::verify_server_authenticator(key, &client_nonce, &server_nonce, &response[36..])
            .unwrap();
        crypto::derive_session_keys(key, &client_nonce, &server_nonce).unwrap()
    }

    fn send_test_control(
        stream: &mut TcpStream,
        key: &[u8; 32],
        sequence: u64,
        message: &Message,
    ) {
        let record = crypto::seal_control_record(key, sequence, &encode_message(message)).unwrap();
        stream.write_all(&record).unwrap();
    }

    fn read_test_control(stream: &mut TcpStream, key: &[u8; 32], expected: u64) -> Vec<u8> {
        let mut header = [0; 4];
        stream.read_exact(&mut header).unwrap();
        let length = u32::from_le_bytes(header) as usize;
        assert!((24..=64 * 1024 + 24).contains(&length));
        let mut body = vec![0; length];
        stream.read_exact(&mut body).unwrap();
        let sequence = u64::from_le_bytes(body[..8].try_into().unwrap());
        assert_eq!(sequence, expected);
        let mut aad = b"KitVR v2 control".to_vec();
        aad.extend_from_slice(&sequence.to_le_bytes());
        crypto::open(key, sequence, &aad, &body[8..]).unwrap()
    }

    #[test]
    fn secure_idle_control_session_closes_but_messages_extend_its_lifetime() -> io::Result<()> {
        let listener = TcpListener::bind(("127.0.0.1", 0))?;
        let mut client = TcpStream::connect(listener.local_addr()?)?;
        client.set_read_timeout(Some(Duration::from_secs(2)))?;
        let (server, _) = listener.accept()?;
        let udp = Arc::new(media_socket(&server, 0)?);
        let worker = thread::spawn(move || {
            let settings = bridge::Settings::default();
            let audio = audio::AudioSettings { capture: None, mic_output: None };
            let key = [7; 32];
            handle_control(
                server, &key, &settings, &audio, &udp, None,
                #[cfg(any(target_os = "linux", windows))]
                None,
                Duration::from_millis(700),
            )
        });
        let key = [7; 32];
        let keys = authenticate_test_client(&mut client, &key);
        send_test_control(
            &mut client,
            &keys.client_to_server_control,
            0,
            &Message::Hello { version: PROTOCOL_VERSION, name: "test".into() },
        );
        let hello = read_test_control(&mut client, &keys.server_to_client_control, 0);
        let (message, used) = decode_message(&hello).unwrap();
        assert!(matches!(message, Message::Hello { version: PROTOCOL_VERSION, .. }));
        assert!(matches!(decode_message(&hello[used..]).unwrap().0, Message::StreamConfig { .. }));
        for ts_us in 0..3 {
            thread::sleep(Duration::from_millis(300));
            send_test_control(
                &mut client,
                &keys.client_to_server_control,
                ts_us + 1,
                &Message::Ping { ts_us },
            );
            let response = read_test_control(&mut client, &keys.server_to_client_control, ts_us + 1);
            assert_eq!(decode_message(&response).unwrap().0, Message::Pong { ts_us });
        }
        assert_eq!(worker.join().unwrap().unwrap_err().kind(), io::ErrorKind::TimedOut);
        Ok(())
    }

    #[test]
    fn only_tracking_can_establish_the_media_sender() {
        let peer: SocketAddr = "127.0.0.1:5000".parse().unwrap();
        let other_port: SocketAddr = "127.0.0.1:5001".parse().unwrap();
        let other_ip: SocketAddr = "127.0.0.2:5000".parse().unwrap();

        assert!(!accepts_media_sender(PACKET_TYPE_TRACKING, peer, peer.ip(), None, false));
        assert!(accepts_media_sender(PACKET_TYPE_TRACKING, peer, peer.ip(), None, true));
        assert!(!accepts_media_sender(PACKET_TYPE_BODY_TRACKING, peer, peer.ip(), None, true));
        assert!(!accepts_media_sender(PACKET_TYPE_AUDIO_UP, peer, peer.ip(), None, true));
        assert!(!accepts_media_sender(PACKET_TYPE_VIEW_CONFIG, peer, peer.ip(), None, true));
        assert!(!accepts_media_sender(PACKET_TYPE_TRACKING, other_ip, peer.ip(), None, true));
        assert!(accepts_media_sender(PACKET_TYPE_BODY_TRACKING, peer, peer.ip(), Some(peer), true));
        assert!(!accepts_media_sender(PACKET_TYPE_TRACKING, other_port, peer.ip(), Some(peer), true));
    }

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
