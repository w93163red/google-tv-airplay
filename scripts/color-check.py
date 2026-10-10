#!/usr/bin/env python3
"""Checks the colors the device shows against an ffmpeg decode of the same access unit.

usage: color-check.py RECORDING [--at 3.5 5.5 ...] [--keep DIR]

Replays RECORDING (a local file, pushed first, or a file name already in the app's files dir) on
the device, takes a raw screencap at each --at second of replay time, and relaunches the app
normally when done. Each capture is compared with the recording's frames around that moment,
decoded by ffmpeg and laid out as the app lays them out (portrait: Fit, landscape: Fill), once read
as full range and once as limited range BT.709. The frame that matches best is the one on screen.

Per capture it prints the screen minus the full-range reference per channel (mean, in 8-bit
levels), the least-squares gain and offset of screen luma against reference luma, the 1st and 99th
percentile luma of both, and the mean absolute error against each range reading. A range mismatch
shows as gain 255/219 = 1.16 with a large negative offset; scaling and chroma interpolation only add
unbiased noise. Needs ffmpeg and ffprobe on PATH and $ANDROID_SERIAL like adb.

screencap composes on the GPU from the layer's buffer and dataspace. What reaches HDMI comes from the
hardware composer's overlay instead (dumpsys SurfaceFlinger: composition DEVICE), which no capture on a
user build can read, so a pass covers the decoder and the dataspace it tags, not the overlay.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

APP = "dev.airscreen"
FILES = f"/data/user/0/{APP}/files"
SCREEN_W, SCREEN_H = 1920, 1080
REDUCE = 4
HEADER = struct.Struct("<IIqQII")
VIDEO = 2
PASS_MEAN = 3.0
PASS_GAIN = 0.05


def adb(*args, **kw):
    return subprocess.run(["adb", *args], check=True, capture_output=True, **kw).stdout


def read_recording(path):
    """The video access units (Annex-B) and each one's replay time in seconds."""
    units, times, first = [], [], None
    with open(path, "rb") as f:
        assert f.read(8) in (b"AIRSREC1", b"AIRSREC2"), "not a session recording"
        while len(head := f.read(HEADER.size)) == HEADER.size:
            kind, length, arrival, _, _, _ = HEADER.unpack(head)
            payload = f.read(length)
            first = arrival if first is None else first
            if kind == VIDEO:
                units.append(payload)
                times.append((arrival - first) / 1e9)
    return units, times


def frame_sizes(stream):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "frame=width,height", "-of", "json", stream],
                         check=True, capture_output=True).stdout
    return [(f["width"], f["height"]) for f in json.loads(out)["frames"]]


def layout(w, h):
    """(ffmpeg filters from a decoded frame to the reduced screen region, screen crop x, crop width).

    Mirrors streamViewSize: portrait fits the screen height, landscape fills the screen and is centered."""
    if h > w:
        vw = round(SCREEN_H * w / h)
        left = (SCREEN_W - vw) // 2
        cw = (vw - 2) // REDUCE * REDUCE
        return f"scale={vw}:{SCREEN_H},crop={cw}:{SCREEN_H}:1:0", left + 1, cw
    vw = round(SCREEN_H * w / h)
    return f"scale={vw}:{SCREEN_H},crop={SCREEN_W}:{SCREEN_H}:{(vw - SCREEN_W) // 2}:0", 0, SCREEN_W


def ffmpeg_rgb(inputs, vf, w, h):
    raw = subprocess.run(["ffmpeg", "-v", "error", *inputs, "-vf", vf, "-fps_mode", "passthrough",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], check=True, capture_output=True).stdout
    size = w * h * 3
    return [raw[i:i + size] for i in range(0, len(raw), size)]


def references(work, units, first, lo, hi, size, place, cw):
    """Frames lo..hi read as full and as limited range BT.709, laid out and reduced like the screen region.

    Decodes from `first`, where the frames' resolution starts, so references resolve and frame numbers
    survive ffmpeg resetting its filter graph on a resolution change. The decoded YUV is then
    reinterpreted with an explicit range, because ffmpeg otherwise follows the stream's own VUI."""
    stream, yuv = os.path.join(work, "segment.es"), os.path.join(work, "segment.yuv")
    with open(stream, "wb") as f:
        f.writelines(units[first:hi + 1])
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", stream, "-vf", f"select='gte(n\\,{lo - first})'",
                    "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "yuvj420p", yuv], check=True)
    reduce = f"scale={cw // REDUCE}:{SCREEN_H // REDUCE}:flags=area"
    return {rng: ffmpeg_rgb(["-f", "rawvideo", "-pix_fmt", "yuv420p", "-color_range", rng, "-colorspace", "bt709",
                             "-s", "%dx%d" % size, "-i", yuv],
                            f"scale=out_range=full:out_color_matrix=bt709,format=rgb24,{place},{reduce}",
                            cw // REDUCE, SCREEN_H // REDUCE)
            for rng in ("pc", "tv")}


def screen_region(raw_path, x, cw):
    with open(raw_path, "rb") as f:
        data = f.read()
    w, h, fmt = struct.unpack_from("<III", data)
    assert (w, h, fmt) == (SCREEN_W, SCREEN_H, 1), f"unexpected screencap {w}x{h} format {fmt}"
    pixels = data[len(data) - w * h * 4:]
    with tempfile.NamedTemporaryFile(suffix=".rgba") as tmp:
        tmp.write(pixels)
        tmp.flush()
        vf = f"crop={cw}:{SCREEN_H}:{x}:0,scale={cw // REDUCE}:{SCREEN_H // REDUCE}:flags=area"
        return ffmpeg_rgb(["-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{w}x{h}", "-i", tmp.name],
                          vf, cw // REDUCE, SCREEN_H // REDUCE)[0]


def mae(a, b, step=1):
    return sum(abs(a[i] - b[i]) for i in range(0, len(a), step)) / len(range(0, len(a), step))


def luma(px):
    return [0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2] for i in range(0, len(px), 3)]


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def compare(screen, ref):
    n = len(screen) // 3
    mean = [sum(screen[i] - ref[i] for i in range(c, len(screen), 3)) / n for c in range(3)]
    ys, yr = luma(screen), luma(ref)
    mr, ms = sum(yr) / n, sum(ys) / n
    var = sum((r - mr) ** 2 for r in yr)
    gain = sum((r - mr) * (s - ms) for r, s in zip(yr, ys)) / var if var else float("nan")
    return mean, gain, ms - gain * mr, (pct(ys, 1), pct(ys, 99)), (pct(yr, 1), pct(yr, 99))


def capture(name, t0, at):
    """Waits until replay time `at`, screencaps, returns the replay-time span the grab happened in."""
    delay = t0 + at - float(adb("shell", "date +%s.%N").decode())
    if delay > 0:
        time.sleep(delay)
    out = adb("shell", f"date +%s.%N; screencap /data/local/tmp/{name}; date +%s.%N").decode().split()
    return float(out[0]) - t0, float(out[1]) - t0


def wait_for_replay(since):
    """Returns the device epoch of replay time 0: REPLAY_LEAD_NS after the 'replaying' log line."""
    log = subprocess.Popen(["adb", "logcat", "-v", "epoch", "-T", since, "-s", "AirScreen:I"],
                           stdout=subprocess.PIPE, text=True)
    try:
        for line in log.stdout:
            if "cannot read recording" in line:
                sys.exit(line.strip())
            if "replaying" in line:
                return float(line.split()[0]) + 0.2
    finally:
        log.kill()
    sys.exit("logcat ended before the replay started")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("--at", type=float, nargs="+", default=[3.5, 5.5, 7.2, 13.4, 18.4])
    ap.add_argument("--keep", help="directory to keep screencaps and references in")
    args = ap.parse_args()
    work = args.keep or tempfile.mkdtemp(prefix="color-check-")
    os.makedirs(work, exist_ok=True)

    name = os.path.basename(args.recording)
    local = args.recording if os.path.isfile(args.recording) else os.path.join(work, name)
    if os.path.isfile(args.recording):
        adb("push", args.recording, f"/data/local/tmp/{name}")
        adb("shell", f"cat /data/local/tmp/{name} | run-as {APP} sh -c 'cat > files/{name}' "
                     f"&& rm /data/local/tmp/{name}")
    else:
        with open(local, "wb") as f:
            f.write(adb("exec-out", "run-as", APP, "cat", f"files/{name}"))
    annexb = os.path.join(work, "video.es")
    units, times = read_recording(local)
    with open(annexb, "wb") as f:
        f.writelines(units)
    sizes = frame_sizes(annexb)
    assert len(sizes) == len(times), f"{len(sizes)} decoded frames for {len(times)} access units"

    since = "%.3f" % float(adb("shell", "date +%s.%N").decode())
    adb("shell", "am", "start", "-S", "-n", f"{APP}/.MainActivity", "--es", "replay", f"{FILES}/{name}")
    t0 = wait_for_replay(since)
    spans = []
    try:
        for i, at in enumerate(args.at):
            spans.append(capture(f"color-{i}.raw", t0, at))
    finally:
        adb("shell", "am", "start", "-S", "-n", f"{APP}/.MainActivity")
    for i in range(len(spans)):
        adb("pull", f"/data/local/tmp/color-{i}.raw", os.path.join(work, f"color-{i}.raw"))
        adb("shell", "rm", f"/data/local/tmp/color-{i}.raw")

    print(f"pass: |screen - full-range reference| mean < {PASS_MEAN} levels per channel, luma gain 1 +- {PASS_GAIN}")
    failed = False
    for i, (start, end) in enumerate(spans):
        # Video renders at most 500 ms after arrival (audio sync); allow the decode and the grab on top.
        cands = [k for k, t in enumerate(times) if start - 0.7 <= t <= end]
        portrait = [k for k in cands if sizes[k][1] > sizes[k][0]]
        cands = portrait if len(portrait) * 2 >= len(cands) else [k for k in cands if k not in portrait]
        lo, hi = cands[0], cands[-1]
        first = lo
        while first > 0 and sizes[first - 1] == sizes[lo]:
            first -= 1
        place, x, cw = layout(*sizes[lo])
        screen = screen_region(os.path.join(work, f"color-{i}.raw"), x, cw)
        refs = references(work, units, first, lo, hi, sizes[lo], place, cw)
        full, limited = refs["pc"], refs["tv"]
        best = min(range(len(full)), key=lambda k: mae(screen, full[k], 7))
        mean, gain, offset, ys, yr = compare(screen, full[best])
        ok = all(abs(m) < PASS_MEAN for m in mean) and abs(gain - 1) < PASS_GAIN
        failed |= not ok
        w, h = sizes[lo + best]
        print(f"t={start:5.2f}-{end:5.2f}s AU {lo + best} {w}x{h} {'portrait Fit' if h > w else 'landscape Fill'}: "
              f"mean screen-ref R {mean[0]:+.1f} G {mean[1]:+.1f} B {mean[2]:+.1f}  "
              f"luma gain {gain:.3f} offset {offset:+.1f}  "
              f"Y p1/p99 screen {ys[0]:.0f}/{ys[1]:.0f} ref {yr[0]:.0f}/{yr[1]:.0f}  "
              f"MAE full {mae(screen, full[best]):.1f} limited {mae(screen, limited[best]):.1f}  "
              f"{'PASS' if ok else 'FAIL'}")
    print(f"captures and stream: {work}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
