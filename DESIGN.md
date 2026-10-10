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

### Native receiver config (passed once at start)

```c
typedef struct {
    const char *name;          /* TV device name, used as the AirPlay name */
    uint8_t hw_addr[6];        /* stable per-install MAC-like id */
    uint16_t width, height, refresh_hz, max_fps;
    bool h265;                 /* advertise feature bit 42 */
    const char *keyfile;       /* persisted ed25519 key path in app files dir */
} receiver_config_t;
```

Kotlin picks width, height, and `h265` from the real display mode and decoder capabilities. On the Streamer that is 3840x2160@60 with HEVC. On the emulator it is 1920x1080 H.264.

### Fixed network ports

Fixed ports make emulator forwarding and firewall reasoning deterministic. TCP 7000 (RTSP/HTTP, also the AirPlay port) and 7100 (mirror data). UDP 7011 (timing), 6001 (control), 6000 (audio data). These match UxPlay's `-p` defaults.

## Native module boundaries (`app/src/main/cpp`)

| File | Owns |
|---|---|
| `receiver.c` | raop and dnssd lifecycle, the `raop_callbacks_t` table, connection reset handling |
| `dnssd_android.c` | the dnssd private backend. Builds TXT records like UxPlay's mdnsd backend, forwards register and unregister to Kotlin |
| `video_renderer.c` | AMediaCodec decoder on the `ANativeWindow`. H.264 and H.265, codec switch, low-latency config |
| `audio_renderer.c` | AAC-ELD and AAC-LC through AMediaCodec, ALAC through Apple's decoder, PCM out through AAudio |
| `jni_bridge.c` | JNI entry points and the event upcall. Nothing else |

## Latency rules

1. Decoder format sets `low-latency=1`, `priority=0`, `max-width` and `max-height` to the advertised display so rotation and resolution changes reconfigure without a new codec.
2. Input is queued from the network callback as soon as it arrives. No reordering buffer, no pacing.
3. Output is released with `render=true` as soon as it is dequeued. No presentation-time scheduling.
4. Audio output uses AAudio `PERFORMANCE_MODE_LOW_LATENCY` and a small jitter buffer.
5. The `SurfaceView` (not `TextureView`) is sized to the stream aspect ratio, letterboxed on black.

## Verification

1. Emulator: protocol and functional path. `scripts/emu-forward.sh` forwards the fixed ports into the emulator and registers a proxy mDNS record on the Mac.
2. Streamer: latency, frame drops, 4K HEVC, 30-minute soak. Numbers come from decoder counters logged by the app, not from eyeballing.
