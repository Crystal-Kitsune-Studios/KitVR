//! Windows audio through cpal (WASAPI). Works with any endpoint Windows exposes, including
//! virtual devices such as Voicemeeter's.
//!
//! Capture: a recording device is used as is. A playback device is captured in loopback mode.
//! Playback: renders microphone audio to a playback device.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::SyncSender;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::SampleFormat;

use super::{RawChunk, SAMPLE_RATE};

fn is_default(want: &str) -> bool {
    want.is_empty() || want.eq_ignore_ascii_case("default")
}

fn name_matches(device: &cpal::Device, want: &str) -> bool {
    device
        .name()
        .map(|n| n.to_lowercase().contains(&want.to_lowercase()))
        .unwrap_or(false)
}

pub fn list() -> Vec<String> {
    let host = cpal::default_host();
    let mut v = Vec::new();
    if let Ok(devs) = host.input_devices() {
        for d in devs {
            if let Ok(n) = d.name() {
                v.push(format!("recording device (capture): {n}"));
            }
        }
    }
    if let Ok(devs) = host.output_devices() {
        for d in devs {
            if let Ok(n) = d.name() {
                v.push(format!("playback device (loopback capture, or microphone output): {n}"));
            }
        }
    }
    v
}

/// Returns the device and whether it must be captured in loopback mode.
fn find_capture(want: &str) -> Result<(cpal::Device, bool), String> {
    let host = cpal::default_host();
    if is_default(want) {
        let d = host.default_output_device().ok_or("no default playback device")?;
        return Ok((d, true));
    }
    // Recording devices first, so a Voicemeeter bus such as "Out B1" is used directly.
    if let Ok(devs) = host.input_devices() {
        for d in devs {
            if name_matches(&d, want) {
                return Ok((d, false));
            }
        }
    }
    if let Ok(devs) = host.output_devices() {
        for d in devs {
            if name_matches(&d, want) {
                return Ok((d, true));
            }
        }
    }
    Err(format!("no audio device matching '{want}' (run with --list-audio)"))
}

pub fn capture(device: &str, stop: &Arc<AtomicBool>, tx: SyncSender<RawChunk>) -> Result<(), String> {
    let (dev, loopback) = find_capture(device)?;
    let name = dev.name().unwrap_or_default();
    let supported = if loopback {
        dev.default_output_config()
    } else {
        dev.default_input_config()
    }
    .map_err(|e| e.to_string())?;
    let rate = supported.sample_rate().0;
    let channels = supported.channels();
    let fmt = supported.sample_format();
    let cfg: cpal::StreamConfig = supported.config();
    println!(
        "audio to headset: {name} ({}, {rate} Hz, {channels} ch, {fmt:?})",
        if loopback { "loopback" } else { "recording device" }
    );

    let err_fn = |e: cpal::StreamError| eprintln!("audio capture error: {e}");
    let built = match fmt {
        SampleFormat::F32 => {
            let tx = tx.clone();
            dev.build_input_stream(
                &cfg,
                move |data: &[f32], _| {
                    let _ = tx.try_send(RawChunk { rate, channels, samples: data.to_vec() });
                },
                err_fn,
                None,
            )
        }
        SampleFormat::I16 => {
            let tx = tx.clone();
            dev.build_input_stream(
                &cfg,
                move |data: &[i16], _| {
                    let samples = data.iter().map(|s| *s as f32 / 32768.0).collect();
                    let _ = tx.try_send(RawChunk { rate, channels, samples });
                },
                err_fn,
                None,
            )
        }
        other => return Err(format!("unsupported sample format {other:?} on {name}")),
    };
    let stream = built.map_err(|e| format!("cannot open {name}: {e}"))?;
    stream.play().map_err(|e| e.to_string())?;

    while !stop.load(Ordering::Relaxed) {
        thread::sleep(Duration::from_millis(100));
    }
    drop(stream);
    Ok(())
}

/// Pulls 48 kHz mono samples from the ring and resamples them to the device rate.
struct Pull {
    ratio: f64, // source frames consumed per output frame
    a: f32,
    b: f32,
    frac: f64,
}

impl Pull {
    fn new(device_rate: u32) -> Self {
        Self { ratio: SAMPLE_RATE as f64 / device_rate as f64, a: 0.0, b: 0.0, frac: 1.0 }
    }

    fn reset(&mut self) {
        self.a = 0.0;
        self.b = 0.0;
        self.frac = 1.0;
    }

    fn next(&mut self, q: &mut VecDeque<f32>) -> f32 {
        while self.frac >= 1.0 {
            self.a = self.b;
            if let Some(v) = q.pop_front() {
                self.b = v;
            }
            self.frac -= 1.0;
        }
        let v = self.a + (self.b - self.a) * self.frac as f32;
        self.frac += self.ratio;
        v
    }
}

fn render(pull: &mut Pull, ring: &Mutex<VecDeque<f32>>, channels: usize, out: &mut [f32]) {
    let channels = channels.max(1);
    let frames = out.len() / channels;
    let mut q = ring.lock().unwrap();
    let need = (frames as f64 * pull.ratio).ceil() as usize + 2;
    if q.len() < need {
        out.fill(0.0); // underrun: play silence until enough audio has arrived
        pull.reset();
        return;
    }
    for frame in out.chunks_mut(channels) {
        let v = pull.next(&mut q);
        frame.fill(v);
    }
}

pub fn playback(device: &str, stop: &Arc<AtomicBool>, ring: Arc<Mutex<VecDeque<f32>>>) -> Result<(), String> {
    let host = cpal::default_host();
    let dev = if is_default(device) {
        host.default_output_device()
    } else {
        host.output_devices().ok().and_then(|mut it| it.find(|d| name_matches(d, device)))
    }
    .ok_or_else(|| format!("no playback device matching '{device}' (run with --list-audio)"))?;

    let name = dev.name().unwrap_or_default();
    let supported = dev.default_output_config().map_err(|e| e.to_string())?;
    let rate = supported.sample_rate().0;
    let channels = supported.channels() as usize;
    let fmt = supported.sample_format();
    let cfg: cpal::StreamConfig = supported.config();
    println!("microphone to PC: {name} ({rate} Hz, {channels} ch, {fmt:?})");

    let mut pull = Pull::new(rate);
    let err_fn = |e: cpal::StreamError| eprintln!("microphone output error: {e}");
    let built = match fmt {
        SampleFormat::F32 => dev.build_output_stream(
            &cfg,
            move |data: &mut [f32], _| render(&mut pull, &ring, channels, data),
            err_fn,
            None,
        ),
        SampleFormat::I16 => {
            let mut scratch: Vec<f32> = Vec::new();
            dev.build_output_stream(
                &cfg,
                move |data: &mut [i16], _| {
                    scratch.resize(data.len(), 0.0);
                    render(&mut pull, &ring, channels, &mut scratch);
                    for (o, s) in data.iter_mut().zip(scratch.iter()) {
                        *o = (s.clamp(-1.0, 1.0) * 32767.0) as i16;
                    }
                },
                err_fn,
                None,
            )
        }
        other => return Err(format!("unsupported sample format {other:?} on {name}")),
    };
    let stream = built.map_err(|e| format!("cannot open {name}: {e}"))?;
    stream.play().map_err(|e| e.to_string())?;

    while !stop.load(Ordering::Relaxed) {
        thread::sleep(Duration::from_millis(100));
    }
    drop(stream);
    Ok(())
}
