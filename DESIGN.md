# AirScreen design

An AirPlay receiver for Google TV Streamer 4K (Android 14, arm64). An iPhone or iPad mirrors its screen to the TV through Control Center → Screen Mirroring. The goals are low latency, no stutter, and full picture quality.

## Scope

In scope for v1:

1. Screen mirroring from iOS / iPadOS / macOS, H.264 and H.265, up to 3840x2160@60.
2. Mirroring audio (AAC-ELD).
3. Audio-only AirPlay (Music, Podcasts) as ALAC or AAC-LC.
4. Discovery under the TV's own device name.

Out of scope for v1: AirPlay video URL casting (HLS), PIN or password pairing, photos.

## Process layout

```
iPhone ──mDNS──> NsdManager (Kotlin)          advertises _airplay._tcp and _raop._tcp
iPhone ──TCP/UDP──> UxPlay raop (C, NDK)      pairing, FairPlay, RTSP, decryption, NTP
                    ├── video_process ──> video renderer (C, AMediaCodec → ANativeWindow)
                    └── audio_process ──> audio renderer (C, AMediaCodec or ALAC → AAudio)
                    └── events ──JNI──> ReceiverState (Kotlin) ──> UI
```

The media hot path never crosses JNI. Every frame goes from the UxPlay callback thread straight into NDK `AMediaCodec`. Kotlin only sees lifecycle events and the `Surface`.

## Data shapes

### Kotlin receiver state (one sealed type, one owner)

```kotlin
sealed interface ReceiverState {
    data object Starting : ReceiverState
    data class Advertising(val name: String, val ip: String) : ReceiverState
    data class Mirroring(val client: String, val codec: VideoCodec, val width: Int, val height: Int) : ReceiverState
    data class AudioOnly(val client: String) : ReceiverState
    data class Failed(val reason: String) : ReceiverState
}
```

The native layer emits `NativeEvent` values (client connected, codec chosen, video size, mirroring started or stopped, connection lost). A reducer `(ReceiverState, NativeEvent) -> ReceiverState` produces the next state. The UI renders the state and nothing else.

The process-level `Receiver` object owns that state and the native receiver. The receiver runs while at least one `MainActivity` is started, so an instance that starts before another stops never restarts or ends a live session, and leaving the app stops it and its advertising. `MainActivity` is `singleTask`, so a launch from the launcher reaches the running instance. `scripts/relaunch-check.sh` checks both on the device.

### Native receiver config (passed once at start)

```c
typedef struct {
    const char *name;          /* TV device name, used as the AirPlay name */
    uint8_t hw_addr[6];        /* stable per-install MAC-like id */
    uint16_t width, height, refresh_hz, max_fps;
    bool h265;                 /* advertise feature bit 42 */
    const char *avc_decoder, *hevc_decoder;  /* MediaCodec names, NULL to pick by type */
    const char *keyfile;       /* persisted ed25519 key path in app files dir */
    const char *files_dir;     /* session recordings */
    const char *replay;        /* debug: replay this recording instead of serving AirPlay */
} receiver_config_t;
```

Kotlin picks width, height, and the decoders from the real display mode and decoder capabilities: per MIME type the hardware decoder that declares `FEATURE_LowLatency`, else the first hardware decoder. `h265` is set when an HEVC decoder exists. On the Streamer that is 3840x2160@60 with HEVC. On the emulator it is 1920x1080 H.264.

### Fixed network ports

Fixed ports make emulator forwarding and firewall reasoning deterministic. TCP 7000 (RTSP/HTTP, also the AirPlay port) and 7100 (mirror data). UDP 7011 (timing), 6001 (control), 6000 (audio data). These match UxPlay's `-p` defaults.

## Native module boundaries (`app/src/main/cpp`)

| File | Owns |
|---|---|
| `receiver.c` | raop and dnssd lifecycle, the `raop_callbacks_t` table, connection reset handling |
| `dnssd_android.c` | the dnssd private backend. Builds TXT records like UxPlay's mdnsd backend, forwards register and unregister to Kotlin |
| `video_renderer.c` | the access-unit queue and its feeder thread, AMediaCodec decoder on the `ANativeWindow`. H.264 and H.265, codec switch, low-latency config |
| `audio_renderer.c` | AAC-ELD and AAC-LC through AMediaCodec, ALAC through Apple's decoder, PCM out through AAudio |
| `resampler.c` | streaming stereo sample-rate conversion, polyphase windowed sinc |
| `recording.c` | the session recording file format, the recorder, and replay of a recording into the renderers |
| `jni_bridge.c` | JNI entry points and the event upcall. Nothing else |

## Latency rules

1. Decoder format sets `low-latency=1`, `priority=0`, `max-width` and `max-height` to the advertised display so rotation and resolution changes reconfigure without a new codec.
2. Each frame has a render time. While audio plays, it is the moment audio plays the frame's sender timestamp: the frame's sender time plus the audio's (local presentation time minus sender time), averaged over 16 packets so the schedule does not jitter, clamped to at most 500 ms after arrival. Bluetooth audio runs about 300 to 400 ms behind arrival, so video waits for it. Without audio, the render time is when the frame would have been decoded had it arrived on the arrival envelope: its sender time plus the lowest arrival-minus-sender time seen (rising 5 ms per second, so drift and a slower path catch up) plus the decode latency. The network delivers frames in bursts (the iPhone recording: a quarter arrive more than 17 ms and a tenth more than 48 ms behind the envelope), and pacing by sender time undoes the bunching that a burst would otherwise carry to the screen.
3. Video waits for its render time as compressed access units, in front of the decoder, never as decoded frames inside it: frames held in the decoder's output use up its output buffers, and then the decoder stops taking input. The mirror thread only copies each access unit into a queue owned by `video_renderer.c` (64 access units and 8 MB, more than the 500 ms clamp holds at 60 fps) and returns, so the decoder never pushes back on TCP. A feeder thread hands each access unit to the decoder at its render time minus the decoder's measured latency (fed to decoded, averaged over 16 frames) minus 10 ms. The MediaTek decoders emit a frame only after one to three later access units have been fed, so an access unit also goes in no later than 10 ms before the render time of the access unit three before it. The feeder waits for an input buffer rather than drop an access unit. If the queue ever fills, the access unit is dropped, and so is everything after it until the next keyframe, which is what those reference; the stats count it as `overflow` and the log says so.
4. Output is released as soon as it is dequeued, with `AMediaCodec_releaseOutputBufferAtTime` at the frame's render time, or now when that has passed. Two frames released for one vsync show only the second, so a frame that would follow the previous one within a refresh is moved to the next vsync, when that is at most two refreshes after its render time (stats: `spread`).
5. Audio output uses AAudio `PERFORMANCE_MODE_LOW_LATENCY` in callback mode at the device's own sample rate, with `resampler.c` converting from 44.1 kHz. On the Streamer that is the only combination measured to get a FAST track: a blocking-write stream at any rate lands on the deep-buffer mixer with a legacy burst of 190 ms or more, a 44.1 kHz callback stream gets a normal track, and `AAudioStreamBuilder_setSampleRateConversionQuality` is in neither NDK r27 nor the device's libaaudio. The AAudio buffer targets 40 ms (at least two bursts). Between the network thread and the callback sits a ring that is primed to 30 ms whenever it runs dry and cut back to 30 ms when it holds more than 90 ms, because arrival and playback run at the same rate and any backlog would otherwise persist as latency. A packet that arrives more than 120 ms later than the stream's played packets did on average (arrival minus sender time) is dropped before decoding: after a pause the sender or UxPlay's resend wait can deliver seconds of audio late and faster than real time, and played it would move the A/V offset by as much. A step in UxPlay's RTP-to-sender-time mapping, seen as a sender-time step that differs from the RTP step, is not lateness: it moves the lateness average and the audio offset at once.
6. The `SurfaceView` (not `TextureView`) keeps the stream aspect ratio. Its layout always fits the whole stream; zoom is a 250 ms scale and translation animation on top, so a zoom change never resizes the surface. `streamPlacement(parent, stream, ZoomMode, ContentRect?)` is the one pure function that decides it. The remote's OK button toggles `ZoomMode`, persisted:
   - `Full` fits the whole stream and never crops.
   - `Smart`, the default, fits the detected picture inside letterbox or pillarbox bars to the screen, so a 16:9 video in an iPad's 4:3 stream fills the TV. Without bars, a stream wider than the screen (iPhone landscape, ~2.17:1) fills it by cropping the sides, and any other stream fits, because an iPad keeps its status bar and tab bars at the top and bottom. Bars only ever zoom in further than that rule.

   While mirroring in `Smart`, `ContentSampler` copies the surface into a 96 px wide bitmap with `PixelCopy` every 500 ms on its own thread. `LetterboxDetector` reads the bars from its luma: equal pairs of black edge rows or columns, at least 2% of the axis, with a non-dark center so a dark scene never counts. The `ZoomState` machine zooms in once the same bars hold for 1.5 s and out once they show picture for 1 s, so player controls flashing over a bar do not flap it.

## Verification

1. Emulator: protocol and functional path. `scripts/emu-forward.sh` forwards the fixed ports into the emulator and registers a proxy mDNS record on the Mac.
2. Streamer: latency, frame drops, 4K HEVC, 30-minute soak. Numbers come from decoder counters logged by the app, not from eyeballing.
3. Record and replay. With `adb shell setprop debug.airscreen.record 1` set before a client connects, the receiver writes every access unit and audio packet it hands to the renderers, with its local arrival time and sender NTP time, to `files/rec-<epoch>.bin`, audio packets with their RTP timestamp. `scripts/rec-add-rtp.py` converts a recording made before RTP timestamps were kept. A debug build replays such a file through the same renderers, Surface and AAudio at the original pacing: `scripts/replay.sh <recording>` runs it and summarizes the `AirScreenStats` lines. `scripts/synth-recording.py` builds a synthetic recording when no real one exists. `scripts/color-check.py <recording>` screencaps a replay and compares it with ffmpeg's decode of the same frame read as full and as limited range.

`AirScreenStats` prints once per second. Video: `pushed` is access units queued, `in` fed to the decoder, `overflow` dropped by a full queue, `push_ms` the time the mirror thread spends handing over an access unit, `decode_ms` is fed to decoded and `lead` the current feed lead (decode latency plus margin), `late` counts frames decoded after their render time, `glass_ms` is arrival to the render time handed to the surface (the MediaTek decoders never report actual render times), and `av_offset_ms` is when audio plays a sender timestamp minus when video renders it, positive when audio is late. Audio: `out_latency_ms` is from a write until its last frame plays, extrapolated from `AAudioStream_getTimestamp`, so it includes the HAL and Bluetooth path.
