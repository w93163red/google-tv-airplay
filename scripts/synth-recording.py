#!/usr/bin/env python3
"""Builds a synthetic AirScreen session recording (see app/src/main/cpp/recording.h).

Video is a test pattern that flashes white while the audio beeps, for 100 ms each second, as
H.264 from Apple's VideoToolbox encoder (the encoder family iOS mirroring uses), one IDR at
the start, parameter sets prepended to it and 4-byte start codes, as UxPlay hands them over. Audio is
AAC-LC (ct=4, 1024 samples per packet): ffmpeg cannot write AAC-ELD with the 480-sample frames
mirroring uses. Arrival times follow the sender clock plus a fixed network delay and random jitter.

usage: synth-recording.py OUT.bin [--seconds 60] [--fps 30] [--size 1920x886] [--level 40] [--jitter-ms 8]
"""
import argparse
import os
import random
import struct
import subprocess
import tempfile

REC_VIDEO_CODEC, REC_VIDEO, REC_AUDIO_FORMAT, REC_AUDIO = 1, 2, 4, 5
VIDEO_CODEC_H264 = 1
SAMPLE_RATE, SPF = 44100, 1024


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def nal_units(stream):
    """Yields NAL payloads (without start codes) of an Annex-B stream."""
    starts = []
    i = 0
    while True:
        i = stream.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i + 3)
        i += 3
    for n, start in enumerate(starts):
        end = starts[n + 1] - 3 if n + 1 < len(starts) else len(stream)
        while end > start and stream[end - 1] == 0:
            end -= 1
        yield stream[start:end]


def access_units(stream):
    """Groups NALs into access units: SPS/PPS start a new one, as does a slice with first_mb_in_slice 0."""
    au, has_slice = [], False
    for nal in nal_units(stream):
        kind = nal[0] & 0x1F
        first_slice = kind in (1, 5) and nal[1] & 0x80
        if has_slice and (kind in (7, 8) or first_slice):
            yield au
            au, has_slice = [], False
        if kind in (1, 5, 7, 8):
            au.append(nal)
            has_slice |= kind in (1, 5)
    if has_slice:
        yield au


def adts_frames(data):
    i = 0
    while i + 7 <= len(data):
        assert data[i] == 0xFF and data[i + 1] & 0xF0 == 0xF0, "lost ADTS sync"
        header = 7 if data[i + 1] & 1 else 9
        length = ((data[i + 3] & 3) << 11) | (data[i + 4] << 3) | (data[i + 5] >> 5)
        yield data[i + header:i + length]
        i += length


def record(kind, arrival_ns, payload=b"", ntp_remote=0, a=0, b=0):
    return struct.pack("<IIqQII", kind, len(payload), arrival_ns, ntp_remote, a, b) + payload


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--size", default="1920x886")
    p.add_argument("--level", default="40", help="H.264 level_idc, e.g. 40 or 51")
    p.add_argument("--jitter-ms", type=float, default=8)
    args = p.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        h264, aac = os.path.join(tmp, "v.h264"), os.path.join(tmp, "a.aac")
        # The picture flashes white for the first 100 ms of every second, when the audio beeps: an A/V sync check by eye or camera.
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={args.size}:rate={args.fps}:duration={args.seconds},"
               "drawbox=color=white:t=fill:enable='lt(mod(t,1),0.1)'",
               "-c:v", "h264_videotoolbox", "-realtime", "1", "-profile:v", "high", "-level", args.level, "-bf", "0",
               "-g", str(args.fps * args.seconds), "-b:v", "8M", "-bsf:v", "h264_mp4toannexb", h264)
        ffmpeg("-f", "lavfi", "-i",
               f"sine=frequency=1000:sample_rate={SAMPLE_RATE}:duration={args.seconds},"
               "volume='if(lt(mod(t,1),0.1),1,0)':eval=frame",
               "-ac", "2", "-c:a", "aac_at", "-profile:a", "39", "-b:a", "128k", "-f", "adts", aac)
        aus = list(access_units(open(h264, "rb").read()))
        packets = list(adts_frames(open(aac, "rb").read()))

    rng = random.Random(1)
    remote0, local0, network_ns = 1_700_000_000_000_000_000, 1_000_000_000, 20_000_000
    jitter = lambda: int(rng.uniform(0, args.jitter_ms) * 1e6)
    events = [(local0 - 2, record(REC_VIDEO_CODEC, local0 - 2, a=VIDEO_CODEC_H264)),
              (local0 - 1, record(REC_AUDIO_FORMAT, local0 - 1, a=4, b=SPF))]
    for i, au in enumerate(aus):
        t = i * 1_000_000_000 // args.fps
        arrival = local0 + t + network_ns + jitter()
        payload = b"".join(b"\x00\x00\x00\x01" + nal for nal in au)
        events.append((arrival, record(REC_VIDEO, arrival, payload, remote0 + t)))
    for j, packet in enumerate(packets):
        t = j * SPF * 1_000_000_000 // SAMPLE_RATE
        arrival = local0 + t + network_ns + jitter()
        events.append((arrival, record(REC_AUDIO, arrival, packet, remote0 + t, a=j * SPF)))
    events.sort(key=lambda e: e[0])
    with open(args.out, "wb") as f:
        f.write(b"AIRSREC2")
        for _, rec in events:
            f.write(rec)
    print(f"{args.out}: {len(aus)} video access units, {len(packets)} audio packets")


if __name__ == "__main__":
    main()
