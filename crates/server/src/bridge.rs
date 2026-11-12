//! ffmpeg bridge: spawns ffmpeg to capture and encode HEVC, reads the Annex B stream from its
//! stdout, splits it into access units, and sends each one using the KitVR video packets.
//!
//! This is the first working path on both Windows and Linux. It trades some latency for
//! simplicity: an access unit is only known to be complete when the next one starts, which
//! costs about one frame of delay, and keyframe requests cannot be honored (the stream uses
//! a fixed GOP instead). The native backends in `platform/` replace this later.

use std::io::{self, Read};
use std::net::{SocketAddr, UdpSocket};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(target_os = "linux")]
use std::sync::Arc;

use kitvr_core::proto::video::{packetize_slice, SliceInfo};
use kitvr_core::proto::MAX_PACKET_PAYLOAD;

#[derive(Debug, Clone)]
pub enum Source {
    TestPattern,
    Desktop,
    File(String),
    #[cfg(target_os = "linux")]
    Wayland(Arc<crate::wayland::WaylandCapture>),
    /// Frames captured from vrcompositor by the KitVR Vulkan layer (Linux only).
    #[cfg(target_os = "linux")]
    SteamVr(Arc<crate::steamvr::SteamVrRing>),
}

#[cfg(target_os = "linux")]
pub fn is_wayland_session() -> bool {
    match std::env::var("XDG_SESSION_TYPE") {
        Ok(session) if !session.is_empty() => session.eq_ignore_ascii_case("wayland"),
        _ => std::env::var_os("WAYLAND_DISPLAY").is_some(),
    }
}

#[derive(Debug, Clone)]
pub struct Settings {
    pub source: Source,
    pub encoder: String,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate_kbps: u32,
    pub stereo_sbs: bool,
    /// Frames between keyframes. Also the worst-case recovery time after packet loss.
    pub gop: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            source: Source::TestPattern,
            encoder: "libx265".to_string(),
            width: 1920,
            height: 1080,
            fps: 60,
            bitrate_kbps: 40_000,
            stereo_sbs: false,
            gop: 60,
        }
    }
}

fn v(items: &[&str]) -> Vec<String> {
    items.iter().map(|s| s.to_string()).collect()
}

pub fn ffmpeg_args(s: &Settings) -> Vec<String> {
    let fps = s.fps.to_string();
    let mut a = v(&["-hide_banner", "-loglevel", "warning"]);
    if s.encoder == "hevc_vaapi" {
        a.extend(v(&["-vaapi_device", "/dev/dri/renderD128"]));
    }

    match &s.source {
        Source::TestPattern => {
            a.extend(v(&["-re", "-f", "lavfi", "-i"]));
            a.push(format!("testsrc2=size={}x{}:rate={}", s.width, s.height, s.fps));
        }
        Source::File(path) => {
            a.extend(v(&["-re", "-stream_loop", "-1", "-i"]));
            a.push(path.clone());
        }
        Source::Desktop => {
            if cfg!(windows) {
                a.extend(v(&["-f", "gdigrab", "-framerate", fps.as_str(), "-i", "desktop"]));
            } else {
                let display = std::env::var("DISPLAY").unwrap_or_else(|_| ":0".to_string());
                a.extend(v(&["-f", "x11grab", "-framerate", fps.as_str(), "-i", display.as_str()]));
            }
        }
        #[cfg(target_os = "linux")]
        Source::Wayland(_) => {
            a.extend(v(&["-f", "rawvideo", "-pixel_format", "rgb0", "-video_size"]));
            a.push(format!("{}x{}", s.width, s.height));
            a.extend(v(&["-framerate", fps.as_str(), "-i", "pipe:0"]));
        }
        #[cfg(target_os = "linux")]
        Source::SteamVr(ring) => {
            // The input geometry is whatever the layer captured; --width and --height scale it.
            a.extend(v(&["-f", "rawvideo", "-pixel_format", ring.pixel_format(), "-video_size"]));
            a.push(format!("{}x{}", ring.width(), ring.height()));
            a.extend(v(&["-framerate", fps.as_str(), "-i", "pipe:0"]));
        }
    }

    let mut vf = format!("scale={}:{},fps={}", s.width, s.height, s.fps);
    // A captured framebuffer is already the right way up; only captured desktop frames are flipped.
    #[cfg(target_os = "linux")]
    let flip = !matches!(s.source, Source::SteamVr(_));
    #[cfg(not(target_os = "linux"))]
    let flip = true;
    if flip {
        vf.push_str(",vflip");
    }
    let mut enc: Vec<String> = Vec::new();
    match s.encoder.as_str() {
        "hevc_nvenc" => enc.extend(v(&["-c:v", "hevc_nvenc", "-preset", "p1", "-tune", "ull", "-rc", "cbr", "-zerolatency", "1", "-delay", "0"])),
        "hevc_amf" => enc.extend(v(&["-c:v", "hevc_amf", "-usage", "ultralowlatency", "-rc", "cbr"])),
        "hevc_qsv" => enc.extend(v(&["-c:v", "hevc_qsv", "-preset", "veryfast", "-look_ahead", "0", "-async_depth", "1"])),
        "hevc_vaapi" => {
            enc.extend(v(&["-c:v", "hevc_vaapi"]));
            vf.push_str(",format=nv12,hwupload");
        }
        "libx265" => {
            enc.extend(v(&["-c:v", "libx265", "-preset", "ultrafast", "-tune", "zerolatency"]));
            enc.push("-x265-params".to_string());
            enc.push(format!(
                "repeat-headers=1:bframes=0:keyint={g}:min-keyint={g}:scenecut=0",
                g = s.gop
            ));
        }
        other => enc.extend(v(&["-c:v", other])),
    }

    a.push("-vf".to_string());
    a.push(vf);
    if s.encoder != "hevc_vaapi" {
        a.extend(v(&["-pix_fmt", "yuv420p"]));
    }
    a.extend(enc);

    let bufsize = ((s.bitrate_kbps as f64 / s.fps.max(1) as f64 * 1.5) as u32).max(1);
    a.extend(v(&["-bf", "0", "-g"]));
    a.push(s.gop.to_string());
    a.push("-b:v".to_string());
    a.push(format!("{}k", s.bitrate_kbps));
    a.push("-maxrate".to_string());
    a.push(format!("{}k", s.bitrate_kbps));
    a.push("-bufsize".to_string());
    a.push(format!("{}k", bufsize));

    // dump_extra repeats VPS/SPS/PPS on keyframes, hevc_metadata inserts an access unit
    // delimiter at the start of every frame, which is how we find frame boundaries.
    a.extend(v(&[
        "-bsf:v",
        "dump_extra=freq=keyframe,hevc_metadata=aud=insert",
        "-flush_packets",
        "1",
        "-an",
        "-f",
        "hevc",
        "pipe:1",
    ]));
    a
}

/// Splits an Annex B HEVC byte stream into access units using AUD NAL units (type 35).
pub struct AuSplitter {
    buf: Vec<u8>,
}

impl AuSplitter {
    pub fn new() -> Self {
        Self { buf: Vec::new() }
    }

    /// Feeds bytes and returns every access unit that is now known to be complete.
    pub fn push(&mut self, data: &[u8]) -> Vec<Vec<u8>> {
        self.buf.extend_from_slice(data);
        let starts = find_aud_starts(&self.buf);
        let mut out = Vec::new();
        if starts.len() < 2 {
            return out;
        }
        for w in starts.windows(2) {
            out.push(self.buf[w[0]..w[1]].to_vec());
        }
        let last = *starts.last().unwrap();
        self.buf.drain(..last);
        out
    }
}

fn find_aud_starts(buf: &[u8]) -> Vec<usize> {
    let mut v = Vec::new();
    let mut i = 0;
    while i + 3 < buf.len() {
        if buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 {
            let nal_type = (buf[i + 3] >> 1) & 0x3f;
            if nal_type == 35 {
                let start = if i > 0 && buf[i - 1] == 0 { i - 1 } else { i };
                v.push(start);
            }
            i += 3;
        } else {
            i += 1;
        }
    }
    v
}

/// True if the access unit contains an IRAP picture (NAL types 16 to 21).
pub fn is_keyframe(au: &[u8]) -> bool {
    let mut i = 0;
    while i + 3 < au.len() {
        if au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1 {
            let nal_type = (au[i + 3] >> 1) & 0x3f;
            if (16..=21).contains(&nal_type) {
                return true;
            }
            i += 3;
        } else {
            i += 1;
        }
    }
    false
}

/// Runs ffmpeg and streams its output to `dest` until `stop` is set or ffmpeg exits.
pub fn run(
    settings: &Settings,
    sock: &UdpSocket,
    dest: SocketAddr,
    stop: &AtomicBool,
) -> io::Result<()> {
    let ffmpeg = std::env::var("KITVR_FFMPEG").unwrap_or_else(|_| "ffmpeg".to_string());
    let args = ffmpeg_args(settings);
    println!("{} {}", ffmpeg, args.join(" "));
    let mut capture: Option<std::process::Child> = None;
    let mut command = Command::new(ffmpeg);
    command.args(&args).stdout(Stdio::piped()).stderr(Stdio::inherit());
    #[cfg(target_os = "linux")]
    if let Source::Wayland(source) = &settings.source {
        let mut producer = start_wayland_capture(source, settings)?;
        let stdout = producer.stdout.take().ok_or_else(|| io::Error::other("missing capture output"))?;
        command.stdin(Stdio::from(stdout));
        configure_child_lifetime(&mut command);
        capture = Some(producer);
    } else if matches!(settings.source, Source::SteamVr(_)) {
        command.stdin(Stdio::piped());
        configure_child_lifetime(&mut command);
    } else {
        command.stdin(Stdio::null());
    }
    #[cfg(not(target_os = "linux"))]
    command.stdin(Stdio::null());

    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            if let Some(mut producer) = capture {
                let _ = producer.kill();
                let _ = producer.wait();
            }
            return Err(error);
        }
    };
    let mut out = child.stdout.take().expect("piped stdout");

    // The capture thread owns ffmpeg's stdin and stops when the ring goes quiet and `stop` is set,
    // or when ffmpeg exits and the pipe breaks.
    #[cfg(target_os = "linux")]
    let feeder = match &settings.source {
        Source::SteamVr(ring) => child.stdin.take().map(|stdin| {
            spawn_ring_feeder(Arc::clone(ring), stdin, settings.fps, StopFlag(stop as *const AtomicBool))
        }),
        _ => None,
    };

    let mut splitter = AuSplitter::new();
    let mut tmp = vec![0u8; 64 * 1024];
    let mut frame_id: u32 = 0;
    let mut result = Ok(());

    'stream: while !stop.load(Ordering::Relaxed) {
        #[cfg(target_os = "linux")]
        if matches!(settings.source, Source::Wayland(_)) {
            use std::os::fd::AsRawFd;
            let mut fd = libc::pollfd { fd: out.as_raw_fd(), events: libc::POLLIN, revents: 0 };
            // Periodically check stop even if the portal or encoder stops producing frames.
            let ready = unsafe { libc::poll(&mut fd, 1, 200) };
            if ready == 0 { continue; }
            if ready < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted { continue; }
                result = Err(error);
                break;
            }
        }
        let n = match out.read(&mut tmp) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                result = Err(e);
                break;
            }
        };
        for au in splitter.push(&tmp[..n]) {
            let info = SliceInfo {
                frame_id,
                slice_idx: 0,
                slice_count: 1,
                keyframe: is_keyframe(&au),
                pose_timestamp_us: 0,
            };
            for p in packetize_slice(&info, &au, MAX_PACKET_PAYLOAD) {
                if let Err(e) = sock.send_to(&p, dest) {
                    result = Err(e);
                    break 'stream;
                }
            }
            frame_id = frame_id.wrapping_add(1);
        }
    }

    if !stop.load(Ordering::Relaxed) && result.is_ok() {
        result = match child.try_wait() {
            Ok(Some(status)) => Err(io::Error::other(format!("ffmpeg exited unexpectedly: {status}"))),
            Ok(None) => Err(io::Error::other("ffmpeg closed its output unexpectedly")),
            Err(error) => Err(error),
        };
    }
    let _ = child.kill();
    let _ = child.wait();
    #[cfg(target_os = "linux")]
    if let Some(handle) = feeder {
        let _ = handle.join();
    }
    if let Some(mut producer) = capture {
        let _ = producer.kill();
        let _ = producer.wait();
    }
    result
}

/// A raw pointer to the caller's stop flag. The feeder thread is always joined before `run`
/// returns, so the reference outlives it.
#[cfg(target_os = "linux")]
struct StopFlag(*const AtomicBool);

#[cfg(target_os = "linux")]
unsafe impl Send for StopFlag {}

#[cfg(target_os = "linux")]
fn spawn_ring_feeder(
    ring: Arc<crate::steamvr::SteamVrRing>,
    mut stdin: std::process::ChildStdin,
    fps: u32,
    stop: StopFlag,
) -> std::thread::JoinHandle<()> {
    use std::io::Write;
    let interval = std::time::Duration::from_micros(1_000_000 / fps.max(1) as u64);
    std::thread::spawn(move || {
        // Capture the wrapper as a whole; a disjoint field capture would grab the raw pointer.
        let stop = stop;
        let mut frame = Vec::new();
        while !unsafe { (*stop.0).load(Ordering::Relaxed) } {
            if !ring.read_frame(&mut frame) {
                std::thread::sleep(interval);
                continue;
            }
            if stdin.write_all(&frame).is_err() {
                break;
            }
        }
        // Dropping stdin lets ffmpeg finish, which ends the read loop in `run`.
        drop(stdin);
    })
}

#[cfg(target_os = "linux")]
fn configure_child_lifetime(command: &mut Command) {
    use std::os::unix::process::CommandExt;
    let parent = std::process::id() as libc::pid_t;
    unsafe {
        command.pre_exec(move || {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0 {
                return Err(io::Error::last_os_error());
            }
            if libc::getppid() != parent {
                return Err(io::Error::other("server exited before capture started"));
            }
            Ok(())
        });
    }
}

#[cfg(target_os = "linux")]
fn start_wayland_capture(source: &crate::wayland::WaylandCapture, settings: &Settings) -> io::Result<std::process::Child> {
    use std::os::fd::AsRawFd;
    use std::os::unix::process::CommandExt;

    let remote = source.open_remote()?;
    let fd = remote.as_raw_fd();
    let mut command = Command::new("gst-launch-1.0");
    command.args([
        "-q", "pipewiresrc", "fd=3", &format!("path={}", source.node()),
        "!", "queue", "leaky=downstream", "max-size-buffers=1", "max-size-bytes=0", "max-size-time=0",
        "!", "videoconvert", "!", "videoscale", "!", "videorate", "!",
        &format!("video/x-raw,format=RGBx,width={},height={},framerate={}/1",
                 settings.width, settings.height, settings.fps),
        "!", "queue", "leaky=downstream", "max-size-buffers=1", "max-size-bytes=0", "max-size-time=0",
        "!", "fdsink", "fd=1", "sync=false",
    ]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
    configure_child_lifetime(&mut command);
    unsafe {
        command.pre_exec(move || {
            if libc::dup2(fd, 3) < 0 || libc::fcntl(3, libc::F_SETFD, 0) < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    command.spawn().map_err(|error| io::Error::new(error.kind(),
        format!("could not start GStreamer PipeWire capture (install gst-launch-1.0 and pipewiresrc): {error}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn access_unit(nal_type: u8, fill: u8, len: usize) -> Vec<u8> {
        let mut v = vec![0, 0, 0, 1, 35 << 1, 1]; // AUD, 4 byte start code
        v.extend_from_slice(&[0, 0, 1, nal_type << 1, 1]);
        v.extend(std::iter::repeat(fill).take(len));
        v
    }

    #[test]
    fn splits_access_units_across_arbitrary_chunks() {
        let a = access_unit(19, 0xAA, 10); // IDR
        let b = access_unit(1, 0xBB, 5); // P frame
        let c = access_unit(1, 0xCC, 5);
        let mut stream = Vec::new();
        stream.extend_from_slice(&a);
        stream.extend_from_slice(&b);
        stream.extend_from_slice(&c);

        let mut splitter = AuSplitter::new();
        let mut got = Vec::new();
        for chunk in stream.chunks(7) {
            got.extend(splitter.push(chunk));
        }
        // The last access unit is only emitted once the next AUD arrives.
        assert_eq!(got.len(), 2);
        assert_eq!(got[0], a);
        assert_eq!(got[1], b);
        assert!(is_keyframe(&got[0]));
        assert!(!is_keyframe(&got[1]));
    }

    #[test]
    fn args_contain_core_pieces() {
        let args = ffmpeg_args(&Settings::default());
        assert!(args.contains(&"libx265".to_string()));
        assert!(args.contains(&"pipe:1".to_string()));
        assert!(args.contains(&"-an".to_string()));
        assert!(args.iter().any(|a| a.starts_with("testsrc2=size=1920x1080")));
    }

    #[test]
    fn nvenc_uses_no_frame_output_delay() {
        let settings = Settings { encoder: "hevc_nvenc".to_string(), ..Default::default() };
        let args = ffmpeg_args(&settings);
        assert!(args.windows(2).any(|pair| pair == ["-delay", "0"]));
    }

    #[test]
    fn low_bitrate_keeps_a_nonzero_encoder_buffer() {
        let settings = Settings { bitrate_kbps: 1, ..Default::default() };
        let args = ffmpeg_args(&settings);
        assert!(args.windows(2).any(|pair| pair == ["-bufsize", "1k"]));
    }
}
