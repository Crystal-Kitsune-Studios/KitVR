//! Audio streaming: PC audio to the headset, and the headset microphone back to the PC.
//!
//! Devices are chosen by a case-insensitive name fragment, so virtual devices such as
//! Voicemeeter's behave like any other device. Version 1 sends raw 16 bit PCM at 48 kHz:
//! stereo to the headset (5 ms per packet) and mono from the microphone.

use std::collections::VecDeque;
use std::net::{SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, RecvTimeoutError, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use kitvr_core::proto::audio::{build_packet, PACKET_TYPE_AUDIO_DOWN};

#[cfg(windows)]
mod wasapi;
#[cfg(windows)]
use wasapi as backend;

#[cfg(not(windows))]
mod pulse;
#[cfg(not(windows))]
use pulse as backend;

pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u8 = 2;
pub const MIC_CHANNELS: u8 = 1;
/// Samples per channel in each packet: 5 ms.
pub const FRAME_SAMPLES: u16 = 240;
/// Microphone audio queued for playback is capped at 100 ms to bound latency.
const MAX_MIC_BUFFER: usize = (SAMPLE_RATE / 10) as usize;

#[derive(Debug, Clone)]
pub struct AudioSettings {
    /// Device to capture and send to the headset. "default" means the default output
    /// (loopback). None disables PC audio.
    pub capture: Option<String>,
    /// Device that receives the headset microphone, for example a Voicemeeter virtual input.
    /// None disables the microphone, because the default output would play it on your speakers.
    pub mic_output: Option<String>,
}

impl Default for AudioSettings {
    fn default() -> Self {
        Self { capture: Some("default".to_string()), mic_output: None }
    }
}

/// Interleaved f32 audio straight from a capture backend, in the device's own format.
pub struct RawChunk {
    pub rate: u32,
    pub channels: u16,
    pub samples: Vec<f32>,
}

pub fn list_devices() -> Vec<String> {
    backend::list()
}

/// Converts any channel count and sample rate to 48 kHz stereo, using linear interpolation.
struct Converter {
    /// Read position in input frames. Frame -1 is `last`, the final frame of the previous chunk.
    pos: f64,
    last: [f32; 2],
}

impl Converter {
    fn new() -> Self {
        Self { pos: 0.0, last: [0.0; 2] }
    }

    fn frame(c: &RawChunk, i: usize) -> [f32; 2] {
        let ch = c.channels as usize;
        let l = c.samples[i * ch];
        let r = if ch >= 2 { c.samples[i * ch + 1] } else { l };
        [l, r]
    }

    fn convert(&mut self, c: &RawChunk, out: &mut Vec<i16>) {
        let ch = c.channels as usize;
        if ch == 0 || c.rate == 0 {
            return;
        }
        let frames = c.samples.len() / ch;
        if frames == 0 {
            return;
        }
        let step = c.rate as f64 / SAMPLE_RATE as f64;
        let mut p = self.pos;
        while p < frames as f64 - 1.0 {
            let i = p.floor() as isize;
            let frac = (p - i as f64) as f32;
            let a = if i < 0 { self.last } else { Self::frame(c, i as usize) };
            let b = Self::frame(c, (i + 1) as usize);
            for k in 0..2 {
                let v = a[k] + (b[k] - a[k]) * frac;
                out.push((v.clamp(-1.0, 1.0) * 32767.0) as i16);
            }
            p += step;
        }
        self.pos = p - frames as f64;
        self.last = Self::frame(c, frames - 1);
    }
}

/// Captures `device` and sends 5 ms packets to `dest` until `stop` is set.
pub fn run_downstream(
    device: &str,
    sock: &UdpSocket,
    dest: SocketAddr,
    stop: &Arc<AtomicBool>,
) -> Result<(), String> {
    let (tx, rx): (SyncSender<RawChunk>, Receiver<RawChunk>) = sync_channel(64);
    let capture = {
        let (stop, device) = (stop.clone(), device.to_string());
        thread::spawn(move || backend::capture(&device, &stop, tx))
    };

    let t0 = Instant::now();
    let frame_len = FRAME_SAMPLES as usize * CHANNELS as usize;
    let mut conv = Converter::new();
    let mut pending: Vec<i16> = Vec::new();
    let mut seq: u16 = 0;

    while !stop.load(Ordering::Relaxed) {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(chunk) => {
                conv.convert(&chunk, &mut pending);
                while pending.len() >= frame_len {
                    let frame: Vec<i16> = pending.drain(..frame_len).collect();
                    let pkt = build_packet(PACKET_TYPE_AUDIO_DOWN, seq, t0.elapsed().as_micros() as u64, &frame);
                    let _ = sock.send_to(&pkt, dest);
                    seq = seq.wrapping_add(1);
                }
            }
            Err(RecvTimeoutError::Timeout) => {
                if capture.is_finished() {
                    break;
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
    capture.join().unwrap_or_else(|_| Err("audio capture thread panicked".to_string()))
}

/// Plays microphone packets (mono, 48 kHz) from `rx` on `device` until `stop` is set.
pub fn run_upstream(device: &str, stop: &Arc<AtomicBool>, rx: Receiver<Vec<i16>>) -> Result<(), String> {
    let ring: Arc<Mutex<VecDeque<f32>>> = Arc::new(Mutex::new(VecDeque::new()));
    let player = {
        let (stop, ring, device) = (stop.clone(), ring.clone(), device.to_string());
        thread::spawn(move || backend::playback(&device, &stop, ring))
    };

    while !stop.load(Ordering::Relaxed) {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(pcm) => {
                let mut q = ring.lock().unwrap();
                q.extend(pcm.iter().map(|s| *s as f32 / 32768.0));
                while q.len() > MAX_MIC_BUFFER {
                    q.pop_front();
                }
            }
            Err(RecvTimeoutError::Timeout) => {
                if player.is_finished() {
                    break;
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
    player.join().unwrap_or_else(|_| Err("audio playback thread panicked".to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chunk(rate: u32, channels: u16, frames: usize) -> RawChunk {
        let mut samples = Vec::new();
        for i in 0..frames {
            for _ in 0..channels {
                samples.push((i as f32 / frames as f32) * 0.5);
            }
        }
        RawChunk { rate, channels, samples }
    }

    #[test]
    fn same_rate_stereo_is_passthrough_length() {
        let mut c = Converter::new();
        let mut out = Vec::new();
        c.convert(&chunk(48_000, 2, 480), &mut out);
        // The last frame is held back for interpolation with the next chunk.
        assert_eq!(out.len(), 479 * 2);
    }

    #[test]
    fn resamples_44k1_to_48k_with_correct_ratio() {
        let mut c = Converter::new();
        let mut out = Vec::new();
        for _ in 0..100 {
            c.convert(&chunk(44_100, 2, 441), &mut out);
        }
        let frames_out = out.len() / 2;
        let expected = 100.0 * 441.0 * 48_000.0 / 44_100.0;
        assert!((frames_out as f64 - expected).abs() < 5.0, "{frames_out} vs {expected}");
    }

    #[test]
    fn mono_is_duplicated_to_both_channels() {
        let mut c = Converter::new();
        let mut out = Vec::new();
        c.convert(&chunk(48_000, 1, 100), &mut out);
        for pair in out.chunks(2) {
            assert_eq!(pair[0], pair[1]);
        }
    }
}
