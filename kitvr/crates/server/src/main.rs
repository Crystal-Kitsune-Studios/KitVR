mod bridge;
mod platform;

use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use kitvr_core::proto::control::{decode_message, encode_message, Codec, Message};
use kitvr_core::proto::tracking::{Tracking, PACKET_TYPE_TRACKING};
use kitvr_core::proto::{DecodeError, CONTROL_PORT, MEDIA_PORT, PROTOCOL_VERSION};

const USAGE: &str = "\
kitvr-server [options]

  --source testpattern|desktop|file:<path>   what to stream (default testpattern)
  --encoder libx265|hevc_nvenc|hevc_amf|hevc_qsv|hevc_vaapi   (default libx265)
  --width N --height N --fps N               (default 1920 1080 60)
  --bitrate-kbps N                           (default 40000)
  --gop N                                    frames between keyframes (default 60)

Requires ffmpeg on PATH, or set KITVR_FFMPEG to its full path.
";

fn parse_args() -> bridge::Settings {
    let mut s = bridge::Settings::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut next = || args.next().unwrap_or_default();
        match a.as_str() {
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
    s
}

fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "kitvr-server".to_string())
}

fn handle_control(
    mut s: TcpStream,
    settings: &bridge::Settings,
    udp: &Arc<UdpSocket>,
) -> io::Result<()> {
    let stop = Arc::new(AtomicBool::new(false));
    let client: Arc<Mutex<Option<SocketAddr>>> = Arc::new(Mutex::new(None));
    let pose_ts = Arc::new(AtomicU64::new(0));
    let mut workers = Vec::new();

    // Receives tracking packets and learns where to send video.
    {
        let (udp, stop, client, pose_ts) = (udp.clone(), stop.clone(), client.clone(), pose_ts.clone());
        workers.push(thread::spawn(move || {
            let mut buf = [0u8; 2048];
            while !stop.load(Ordering::Relaxed) {
                if let Ok((n, from)) = udp.recv_from(&mut buf) {
                    if n > 0 && buf[0] == PACKET_TYPE_TRACKING {
                        if let Ok(t) = Tracking::parse(&buf[..n]) {
                            pose_ts.store(t.timestamp_us, Ordering::Relaxed);
                            let mut c = client.lock().unwrap();
                            if *c != Some(from) {
                                println!("client media address: {from}");
                                *c = Some(from);
                            }
                        }
                    }
                }
            }
        }));
    }

    // Waits for the first tracking packet, then streams until told to stop.
    {
        let (udp, stop, client, pose_ts) = (udp.clone(), stop.clone(), client.clone(), pose_ts.clone());
        let settings = settings.clone();
        workers.push(thread::spawn(move || {
            let dest = loop {
                if stop.load(Ordering::Relaxed) {
                    return;
                }
                if let Some(a) = *client.lock().unwrap() {
                    break a;
                }
                thread::sleep(Duration::from_millis(20));
            };
            println!("streaming to {dest}");
            if let Err(e) = bridge::run(&settings, &udp, dest, &stop, &pose_ts) {
                eprintln!("bridge stopped: {e}");
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
                            let hello = Message::Hello { version: PROTOCOL_VERSION, name: host_name() };
                            let cfg = Message::StreamConfig {
                                codec: Codec::Hevc,
                                width: settings.width as u16,
                                height: settings.height as u16,
                                fps: settings.fps as u16,
                                bitrate_kbps: settings.bitrate_kbps,
                            };
                            let mut out = encode_message(&hello);
                            out.extend_from_slice(&encode_message(&cfg));
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
    let settings = parse_args();
    println!("kitvr-server, platform: {}", platform::NAME);
    println!("native capture:  {:?}", platform::create_capture().err().map(|e| e.to_string()));
    println!("native encoder:  {:?}", platform::create_encoder().err().map(|e| e.to_string()));
    println!("native input:    {:?}", platform::create_input().err().map(|e| e.to_string()));
    println!("using the ffmpeg bridge: {settings:?}");

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
                if let Err(e) = handle_control(s, &settings, &udp) {
                    eprintln!("connection error: {e}");
                }
            }
            Err(e) => eprintln!("accept error: {e}"),
        }
    }
    Ok(())
}
