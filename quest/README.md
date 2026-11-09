# Quest client

Native Android app (C++ via the NDK, no Java) using OpenXR. One APK for Quest 2, 3 and 3S.

## How it works

1. `main.cpp` creates an OpenXR session (OpenGL ES binding, needed only to create the session).
2. `session.cpp` connects to the server over TCP, receives `StreamConfig`, and reassembles UDP
   video packets into access units.
3. When the session is visible, an `XR_KHR_android_surface_swapchain` is created at the stream
   resolution. Its Surface is handed to `decoder.cpp`, so MediaCodec renders frames straight into
   the compositor. The swapchain is submitted as a quad layer 2 m in front of you.
4. If the server enables audio, PC audio plays through AAudio with a small jitter buffer, and
   the microphone is captured and sent back. Microphone permission is requested at runtime.
5. Every frame, the head pose is sent to the server over UDP. The first packet also tells the
   server where to send video.

```
app/src/main/
  AndroidManifest.xml
  cpp/
    main.cpp      OpenXR setup, event loop, quad layer
    session.*     control channel, UDP receive, frame reassembly, keyframe requests
    decoder.*     AMediaCodec wrapper (low-latency HEVC)
    audio.*       AAudio playback of PC audio and microphone capture
    proto.h       C++ port of crates/proto
```

## Build

You need Android Studio (or the Android SDK, NDK and CMake 3.22 from the SDK manager) and JDK 17.

Open the `quest/` folder in Android Studio and let it sync, or from a shell:

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
3. Tell the app where the PC is, either way:
   - launch with an extra:
     `adb shell am start -n com.crystalkitsune.kitvr/android.app.NativeActivity --es host 192.168.1.50`
   - or put the address in a file so you can launch from the Quest library:
     `adb shell mkdir -p /sdcard/Android/data/com.crystalkitsune.kitvr/files`
     `adb shell "echo 192.168.1.50 > /sdcard/Android/data/com.crystalkitsune.kitvr/files/host.txt"`
4. Logs: `adb logcat -s kitvr`

## Things to check first if it does not work

This code was written without being able to build it, so expect to fix small things.

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
