# AirScreen

## 中文说明

AirScreen 是一个运行在 Android TV 和 Google TV 上的 AirPlay 屏幕镜像接收端。iPhone 或 iPad 用“屏幕镜像”把画面和声音投到电视上。Mac 走的是同一套协议，理论上可用，但还没有测试过。

- 系统要求：Android 11 或更高版本，CPU 架构为 armeabi-v7a 或 arm64-v8a。
- 安装：从 [GitHub Releases](https://github.com/w93163red/google-tv-airplay/releases) 下载 `airscreen-<版本>.apk`，用 `adb install` 安装，或者拷到电视上用文件管理器安装。
- 使用：先在电视上打开 AirScreen。然后在 iPhone 或 iPad 上打开控制中心，点“屏幕镜像”，选择电视的名字。这个名字就是电视的设备名称，可以在电视的“设置 > 系统 > 关于”里修改。
- 遥控器：镜像时按确认键，在“智能填满”和“完整画面”之间切换。按返回键或主页键会退出应用，同时结束镜像。
- 已知问题：部分电视在游戏模式下颜色偏艳，原因还在查。用蓝牙耳机或音箱时，画面会等声音，延迟大约 300 到 400 毫秒。不支持 AirPlay 视频链接投送（HLS）。同一时间只接收一台设备。

本项目使用 GPL-3.0 许可证。AirPlay、iPhone、iPad 和 Mac 是 Apple Inc. 的商标。本项目与 Apple 没有关联，也未获得 Apple 的认可。

## What it is

AirScreen is an AirPlay receiver app for Android TV and Google TV. An iPhone or iPad mirrors its screen to the TV through **Screen Mirroring**, and the TV plays the picture and the sound.

It runs the AirPlay protocol code from [UxPlay](https://github.com/FDH2/UxPlay), built with the NDK. Video goes from UxPlay straight to the TV's hardware decoder through `AMediaCodec`, and audio goes out through AAudio. The app is developed on a Google TV Streamer and the Android emulator.

## Features

- Screen mirroring from iPhone and iPad, tested with an iPhone 15 Pro and an iPad Pro. A Mac uses the same protocol and should work, but it is untested. The app offers the sender the TV's display mode, for example 3840x2160 at 60 Hz, when a hardware decoder supports that size, and 1920x1080 otherwise.
- H.264 and H.265 video. The app offers H.265 only when the TV has a hardware HEVC decoder.
- AAC-ELD audio while mirroring. ALAC and AAC-LC for audio-only AirPlay, such as music.
- Audio and video stay in sync, also over Bluetooth. Video waits for the measured latency of the audio output, Bluetooth path included. Without audio, video shows as soon as the network and the decoder allow.
- Smart zoom for letterboxed video. When the mirrored screen shows a video inside black bars, such as a 16:9 video on an iPad's 4:3 screen, the app zooms in so the video fills the TV. The **OK** key on the remote switches between smart zoom and the full picture.
- Low-latency decoding. For each codec the app picks the hardware decoder that declares low-latency support, or else the first hardware decoder, and turns on its low-latency mode. It also asks the display for its low-latency game mode (HDMI ALLM).
- The TV shows up under its own device name.

## Requirements

- An Android TV or Google TV device with Android 11 (API 30) or later.
- An armeabi-v7a or arm64-v8a CPU.
- The sender and the TV on the same local network, with mDNS between them. The receiver listens on TCP ports 7000 and 7100 and on UDP ports 6000, 6001, and 7011.

## Install

Download `airscreen-<version>.apk` from the [Releases page](https://github.com/w93163red/google-tv-airplay/releases). Then install it with one of these two methods.

To install with `adb`, turn on developer options and USB debugging on the TV, then run:

```sh
adb connect <tv-ip-address>
adb install airscreen-<version>.apk
```

To install without a computer, copy the APK to the TV with a USB drive or a file-transfer app. Open it in a file-manager app on the TV. When Android asks, allow that app to install unknown apps.

A newer release installs over an older one. A build you sign yourself has a different signature, so uninstall the release first.

## Use it

1. Open AirScreen on the TV. It shows the TV's name and IP address.
2. On the iPhone or iPad, open **Control Center** and tap **Screen Mirroring**. On a Mac, open **Control Center** and click **Screen Mirroring**.
3. Pick the TV's name.

The name is the TV's device name. On Google TV you change it under **Settings** > **System** > **About** > **Device name**.

The receiver runs only while AirScreen is open. When you leave the app, the receiver stops and the TV leaves the **Screen Mirroring** list.

## Remote keys

| Key | Effect |
|---|---|
| **OK** (center) or **Enter** | While mirroring, switches between smart zoom and the full picture. The app keeps the choice for later sessions. |
| **Back** or **Home** | Leaves the app. The receiver stops and the mirroring session ends. |

## Known limitations

- Through some displays, colors look over-saturated. The cause is not known yet. It appears to depend on the hardware composer (HWC) path or the display's game mode. This is an open issue. To compare without game mode, run `adb shell am start -n dev.airscreen/.MainActivity --ez allm false`. The setting persists, and `--ez allm true` turns game mode back on.
- Latency follows the audio output, because video waits for audio. With Bluetooth headphones or speakers, the picture is roughly 300 to 400 ms behind the sender.
- AirPlay video URL casting (HLS) is not supported, and neither are photos. Use screen mirroring instead.
- One sender at a time. A new sender takes over from the current one.
- There is no PIN or password. Any device on the local network can connect while the app is open.

## Build from source

You need macOS or Linux, JDK 17, and the Android SDK with platform 35, NDK `27.2.12479018`, and CMake `3.31.6`. `app/build.gradle.kts` names the NDK and CMake versions. Install them with `sdkmanager` or the Android Studio SDK Manager.

1. Build OpenSSL's `libcrypto` for both ABIs. The script downloads OpenSSL 3.5.9, checks its SHA-256, and installs static libraries into `third_party/prebuilt/`. It reads `ANDROID_HOME` (default `~/Library/Android/sdk`) and `ANDROID_NDK_ROOT`.

   ```sh
   scripts/build-deps.sh
   ```

2. Build and test the app. Gradle finds the SDK through `ANDROID_HOME` or `sdk.dir` in `local.properties`.

   ```sh
   ./gradlew assembleDebug testDebugUnitTest
   ```

   The APK is `app/build/outputs/apk/debug/app-debug.apk`.

### Release builds

`./gradlew assembleRelease` signs with the key that these environment variables name:

| Variable | Value |
|---|---|
| `AIRSCREEN_KEYSTORE` | Path to the keystore file |
| `AIRSCREEN_KEYSTORE_PASSWORD` | Keystore password |
| `AIRSCREEN_KEY_ALIAS` | Key alias |
| `AIRSCREEN_KEY_PASSWORD` | Key password |

If none of them is set, the build signs with the debug key and prints a warning. If only some are set, the build fails.

`-PversionName=1.2.0` sets the version name. The version code becomes `major * 1000000 + minor * 1000 + patch`, so `1.2.0` is `1002000`. Without the property, the version is `0.1` with code `1`.

### Continuous integration

`.github/workflows/ci.yml` builds the debug APK and runs the unit tests on every push and pull request to `main`. `.github/workflows/release.yml` runs when you push a tag such as `v1.2.0`. It builds a release-signed `airscreen-1.2.0.apk` and publishes it as a GitHub Release. A tag whose name contains `-`, such as `v1.2.0-rc1`, becomes a pre-release. A manual run of the release workflow uploads the APK as a workflow artifact and creates no release.

## Architecture

Kotlin owns discovery, the UI, and zoom. It advertises `_airplay._tcp` and `_raop._tcp` through `NsdManager`, picks the decoders from the display mode, and turns native events into one `ReceiverState` that the UI renders. C code under `app/src/main/cpp` runs UxPlay's receiver, decodes video with `AMediaCodec` onto the `SurfaceView`, and plays audio through AAudio. Media never crosses JNI.

[DESIGN.md](DESIGN.md) describes the process layout, the native module boundaries, and the latency rules.

## Development tools

Use a debug build for this work. Release builds ignore replay requests. The scripts that copy recordings to or from the device use `run-as`, which works only on a debuggable app.

- Session recording. Run `adb shell setprop debug.airscreen.record 1` before a sender connects. The receiver then writes every access unit and audio packet, with its arrival time and sender time, to `files/rec-<epoch>.bin` in the app's data directory.
- `scripts/replay.sh <recording>` replays a recording through the same decoders, surface, and audio output at the original pacing, then summarizes the `AirScreenStats` log lines with `scripts/replay-summary.py`.
- `scripts/synth-recording.py` builds a synthetic recording, a test pattern that flashes while the audio beeps.
- `scripts/zoom-timeline.py <recording> <dir>` prints the smart-zoom decisions the app would make over a recording.
- `scripts/color-check.py <recording>` takes screen captures during a replay and compares them with ffmpeg's decode of the same frames.
- `scripts/present-fps.py` measures the frames the display actually presented, from SurfaceFlinger.
- `scripts/relaunch-check.sh` checks that relaunching the app keeps one receiver and that **Home** stops it.
- `scripts/emu-forward.sh start` makes a receiver in the Android emulator reachable from the local network.
- `scripts/rec-extract.py` writes a recording's video as an Annex-B stream, and `scripts/rec-add-rtp.py` converts recordings from the older format.

The app logs `AirScreenStats` once per second while it receives. Read it with `adb logcat -s AirScreenStats`. [DESIGN.md](DESIGN.md) explains each field.

## License and credits

AirScreen is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

It includes or links this third-party code:

| Component | Use | License |
|---|---|---|
| [UxPlay](https://github.com/FDH2/UxPlay) | AirPlay protocol (`lib/` only, in `third_party/uxplay`) | GPL-3.0, with some files under LGPL-2.1+ |
| [libplist](https://github.com/libimobiledevice/libplist) | Property list parsing (`third_party/libplist`) | LGPL-2.1+ |
| [Apple ALAC](https://github.com/macosforge/alac) | ALAC decoder (`third_party/alac`) | Apache-2.0 |
| [OpenSSL](https://www.openssl.org) 3.5.9 | `libcrypto`, built by `scripts/build-deps.sh` and linked statically | Apache-2.0 |

`third_party/README.md` lists the upstream commits, and `third_party/PATCHES.md` lists the local changes.

## Trademarks

AirPlay, iPhone, iPad, and Mac are trademarks of Apple Inc. This project is not affiliated with or endorsed by Apple Inc.
