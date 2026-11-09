//! Linux audio through PulseAudio-compatible tools (PipeWire provides them on Bazzite and most
//! distributions). Needs `pactl`, `parec` and `pacat` from the pulseaudio-utils package.

use std::collections::VecDeque;
use std::io::{Read, Write};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::SyncSender;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use super::RawChunk;

fn pactl_names(kind: &str) -> Vec<String> {
    let out = match Command::new("pactl").args(["list", "short", kind]).output() {
        Ok(o) => o,
        Err(_) => return Vec::new(),
    };
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|l| l.split('\t').nth(1).map(|s| s.to_string()))
        .collect()
}

pub fn list() -> Vec<String> {
    let mut v = Vec::new();
    for n in pactl_names("sources") {
        v.push(format!("source (capture, '.monitor' entries are what apps play): {n}"));
    }
    for n in pactl_names("sinks") {
        v.push(format!("sink (playback, microphone output): {n}"));
    }
    if v.is_empty() {
        v.push("no devices found; is pactl installed (pulseaudio-utils) and PipeWire running?".to_string());
    }
    v
}

fn resolve(kind: &str, want: &str, default: &str) -> Result<String, String> {
    if want.is_empty() || want.eq_ignore_ascii_case("default") {
        return Ok(default.to_string());
    }
    let w = want.to_lowercase();
    pactl_names(kind)
        .into_iter()
        .find(|n| n.to_lowercase().contains(&w))
        .ok_or_else(|| format!("no {kind} matching '{want}' (run with --list-audio)"))
}

pub fn capture(device: &str, stop: &Arc<AtomicBool>, tx: SyncSender<RawChunk>) -> Result<(), String> {
    let dev = resolve("sources", device, "@DEFAULT_MONITOR@")?;
    println!("audio to headset: {dev} (parec)");
    let mut child = Command::new("parec")
        .arg("--device")
        .arg(&dev)
        .args(["--format=s16le", "--rate=48000", "--channels=2", "--latency-msec=10"])
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("cannot run parec (install pulseaudio-utils): {e}"))?;
    let mut out = child.stdout.take().ok_or("no stdout from parec")?;

    let mut buf = vec![0u8; 960]; // 5 ms of 48 kHz stereo s16
    while !stop.load(Ordering::Relaxed) {
        if out.read_exact(&mut buf).is_err() {
            break;
        }
        let samples: Vec<f32> = buf
            .chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]) as f32 / 32768.0)
            .collect();
        let _ = tx.try_send(RawChunk { rate: 48_000, channels: 2, samples });
    }
    let _ = child.kill();
    let _ = child.wait();
    Ok(())
}

pub fn playback(device: &str, stop: &Arc<AtomicBool>, ring: Arc<Mutex<VecDeque<f32>>>) -> Result<(), String> {
    let dev = resolve("sinks", device, "@DEFAULT_SINK@")?;
    println!("microphone to PC: {dev} (pacat)");
    let mut child = Command::new("pacat")
        .arg("--playback")
        .arg("--device")
        .arg(&dev)
        .args(["--format=s16le", "--rate=48000", "--channels=1", "--latency-msec=20"])
        .stdin(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("cannot run pacat (install pulseaudio-utils): {e}"))?;
    let mut stdin = child.stdin.take().ok_or("no stdin for pacat")?;

    while !stop.load(Ordering::Relaxed) {
        let samples: Vec<f32> = {
            let mut q = ring.lock().unwrap();
            let n = q.len().min(480);
            q.drain(..n).collect()
        };
        if samples.is_empty() {
            thread::sleep(Duration::from_millis(2));
            continue;
        }
        let mut bytes = Vec::with_capacity(samples.len() * 2);
        for s in samples {
            bytes.extend_from_slice(&((s.clamp(-1.0, 1.0) * 32767.0) as i16).to_le_bytes());
        }
        if stdin.write_all(&bytes).is_err() {
            break;
        }
    }
    drop(stdin);
    let _ = child.kill();
    let _ = child.wait();
    Ok(())
}
