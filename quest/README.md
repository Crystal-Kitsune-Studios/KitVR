# Quest client

Android app (Java launcher and C++ via the NDK) using OpenXR. One APK for Quest 2, 3 and 3S.

## How it works

1. `main.cpp` creates an OpenXR session (OpenGL ES binding, needed only to create the session).
2. `session.cpp` connects to the server over TCP, receives `StreamConfig`, and reassembles UDP
   video packets into access units.
3. When the session is visible, an `XR_KHR_android_surface_swapchain` is created at the stream
   resolution. Its Surface is handed to `decoder.cpp`, so MediaCodec renders frames straight into
   the compositor. The swapchain is submitted as a quad layer 2 m in front of you.
4. If the server enables audio, PC audio plays through AAudio with a small jitter buffer, and
   the microphone is captured and sent back. Microphone permission is requested at runtime.
5. Every frame, the head pose and active Touch-controller state are sent to the server over UDP.
  If Touch controllers are not active, OpenXR hand tracking can map palm pose and pinch/grab to
  controller inputs. On runtimes that expose Meta's full-body joint set, selected body trackers
  are also sent to the SteamVR driver. The first head packet tells the server where to send video.
  Hand tracking currently emulates basic controller inputs rather than streaming finger skeletons.
6. An animated connecting scene fills the headset view while waiting for the first video frame,
  including after a control reconnection.
  Optional waiting music plays quietly and fades out when that frame renders. Turn it off with
  **Music while connecting** in the launcher.

If the headset session stops and resumes, the client reconnects the control channel so the server
can learn the new UDP media address. An unresponsive control connection is retried after five
seconds without incoming control traffic.

For desktop mouse control, enable it explicitly on the PC and use the right hand or controller:
move it to move the pointer, pinch or trigger for left-click, and grab or grip for right-click.
Only enable this on a trusted network; the connection is not authenticated.
Body tracking depends on headset and runtime support: a Quest 2 may not expose the body-tracking
extension, and lower-body poses on supported headsets may be inferred rather than measured.

```
app/src/main/
  AndroidManifest.xml
  java/com/crystalkitsune/kitvr/MainActivity.java  launcher UI, host setting, release updates
  res/xml/file_paths.xml                         secure APK sharing for installation
  cpp/
    main.cpp      OpenXR setup, event loop, quad layer
    wait_scene.*  animated immersive connecting scene
    session.*     control channel, UDP receive, frame reassembly, keyframe requests
    decoder.*     AMediaCodec wrapper (low-latency HEVC)
    audio.*       AAudio playback of PC audio and microphone capture
    proto.h       C++ port of crates/proto
```

## Build

You need Android Studio (or the Android SDK, NDK and CMake 3.22 from the SDK manager) and JDK 17.

Open the `quest/` folder in Android Studio and let it sync, or from a shell with the Android SDK,
NDK and CMake installed:

```
gradle wrapper --gradle-version 8.7     # one time, creates gradlew
./gradlew assembleDebug
```

The APK ends up in `app/build/outputs/apk/debug/`.
The application id is `com.crystalkitsune.kitvr`. Change it in `app/build.gradle.kts` and
`kHostFile` in `main.cpp` if you want a different one.

## Run

1. Enable developer mode on the headset and connect adb (`adb connect <quest-ip>:5555`).
2. Install: `adb install -r app/build/outputs/apk/debug/app-debug.apk`
3. Start streaming in KitVR Desktop on the PC. Launch KitVR from the Quest library and select
  **Find PCs** to discover your server over Wi-Fi, or enter
  its IP address manually. Select **Connect**; the address is saved for the next launch. You can
  also launch the immersive client directly with an extra:
  `adb shell am start -n com.crystalkitsune.kitvr/.KitVrActivity --es host 192.168.1.50`
  For direct native launches without an extra, put the address in a file:
  `adb shell mkdir -p /sdcard/Android/data/com.crystalkitsune.kitvr/files`
  `adb shell "echo 192.168.1.50 > /sdcard/Android/data/com.crystalkitsune.kitvr/files/host.txt"`
4. Logs: `adb logcat -s kitvr`

The launcher checks the latest stable GitHub release on startup. When an update is available, choose
**Download and install**; Android may ask you to allow installs from KitVR and always asks you to
confirm the installation. Updates require a signed release APK from the same signing key as the
installed app.

## Things to check first if it does not work

The client has been tested on Quest 2; device-specific issues may still occur.

- **Swapchain creation fails.** `setupVideo()` sets `usageFlags` to color attachment plus sampled.
  If the runtime rejects it, try `0` or only `XR_SWAPCHAIN_USAGE_SAMPLED_BIT`. The spec requires
  format, sample count, face count, array size and mip count to be 0.
- **Manifest values.** `com.oculus.supportedDevices` is `quest2|questpro|quest3|quest3s`. If the
  headset refuses to launch the app in VR mode, check this value against Meta's current docs.
- **Black screen but logs show frames.** Confirm the state reached VISIBLE or FOCUSED
  (`session state 4` or `5` in the log) and that `video ready` was logged.
- **Decoder errors.** Some devices need the `vendor.qti-ext-dec-low-latency.enable` format key.
  Try removing `low-latency` and `operating-rate` in `decoder.cpp` to rule them out.
- **No microphone.** The permission prompt is raised from a native thread through JNI, which
  can fail on some builds. Grant it manually:
  `adb shell pm grant com.crystalkitsune.kitvr android.permission.RECORD_AUDIO`, then relaunch.
- **Crackling or drifting audio.** Playback trims itself above 80 ms of buffered audio and
  re-primes after an underrun. Persistent crackle usually means Wi-Fi packet loss.
- **Loader.** The OpenXR loader AAR is pinned to 1.0.34 so the API version matches what Quest
  runtimes expect. Do not bump it casually.
