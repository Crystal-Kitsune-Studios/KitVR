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
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use kitvr_core::proto::video::{packetize_slice, SliceInfo};
use kitvr_core::proto::MAX_PACKET_PAYLOAD;

#[derive(Debug, Clone)]
pub enum Source {
    TestPattern,
    Desktop,
    File(String),
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
    }

    let mut vf = format!("scale={}:{},fps={}", s.width, s.height, s.fps);
    let mut enc: Vec<String> = Vec::new();
    match s.encoder.as_str() {
        "hevc_nvenc" => enc.extend(v(&["-c:v", "hevc_nvenc", "-preset", "p1", "-tune", "ull", "-rc", "cbr", "-zerolatency", "1"])),
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

    let bufsize = (s.bitrate_kbps as f32 / s.fps.max(1) as f32 * 1.5) as u32;
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
    pose_ts: &AtomicU64,
) -> io::Result<()> {
    let ffmpeg = std::env::var("KITVR_FFMPEG").unwrap_or_else(|_| "ffmpeg".to_string());
    let args = ffmpeg_args(settings);
    println!("{} {}", ffmpeg, args.join(" "));
    let mut child = Command::new(ffmpeg)
        .args(&args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()?;
    let mut out = child.stdout.take().expect("piped stdout");

    let mut splitter = AuSplitter::new();
    let mut tmp = vec![0u8; 64 * 1024];
    let mut frame_id: u32 = 0;
    let mut result = Ok(());

    while !stop.load(Ordering::Relaxed) {
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
                pose_timestamp_us: pose_ts.load(Ordering::Relaxed),
            };
            for p in packetize_slice(&info, &au, MAX_PACKET_PAYLOAD) {
                if let Err(e) = sock.send_to(&p, dest) {
                    result = Err(e);
                }
            }
            frame_id = frame_id.wrapping_add(1);
        }
    }

    let _ = child.kill();
    let _ = child.wait();
    result
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
}
