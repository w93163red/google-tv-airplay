#!/usr/bin/env python3
"""Measures what the display actually showed for AirScreen's video layer, not what the app released.

Samples `dumpsys SurfaceFlinger --latency <layer>` (the last 128 presented frames with their actual
present times) every second and prints, per window of newly presented frames: presented fps, the
frame-interval percentiles, and how many intervals spanned more than one refresh (a repeated frame
on screen) or more than two (a visible hitch).

usage: present-fps.py [seconds]      (ANDROID_SERIAL selects the device)
"""
import os
import re
import subprocess
import sys
import time

ADB = os.path.expanduser("~/Library/Android/sdk/platform-tools/adb")
PENDING = (1 << 63) - 1


def adb(*args):
    return subprocess.run([ADB, "shell", *args], capture_output=True, text=True).stdout


def video_layer():
    names = [l.strip() for l in adb("dumpsys", "SurfaceFlinger", "--list").splitlines()]
    blast = [n for n in names if re.match(r"SurfaceView\[dev\.airscreen/.*\]\(BLAST\)#\d+$", n)]
    if not blast:
        sys.exit("AirScreen's SurfaceView layer is not on screen")
    return blast[-1]


def present_times(layer):
    lines = adb("dumpsys", "SurfaceFlinger", "--latency", f"'{layer}'").split("\n")
    period = int(lines[0])
    times = []
    for line in lines[1:]:
        parts = line.split()
        if len(parts) == 3 and 0 < int(parts[1]) < PENDING:
            times.append(int(parts[1]))
    return period, times


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    layer = video_layer()
    print(f"layer {layer}")
    seen = 0
    end = time.time() + duration
    while time.time() < end:
        time.sleep(1)
        period, times = present_times(layer)
        fresh = [t for t in times if t > seen]
        if seen == 0 or len(fresh) < 2:
            seen = max(times, default=seen)
            print("no new frames" if seen and len(fresh) < 2 else "warming up")
            continue
        intervals = [b - a for a, b in zip([seen] + fresh, fresh)]
        span = (fresh[-1] - seen) / 1e9
        seen = fresh[-1]
        ms = [i / 1e6 for i in intervals]
        repeats = sum(1 for i in intervals if i > 1.5 * period)
        hitches = sum(1 for i in intervals if i > 2.5 * period)
        print(f"presented {len(fresh) / span:5.1f} fps  interval ms p50 {pct(ms, 50):5.1f} p90 {pct(ms, 90):5.1f} "
              f"max {max(ms):6.1f}  >1 refresh {repeats:3d}  >2 refresh {hitches:3d}  (refresh {period / 1e6:.2f} ms)")


sys.stdout.reconfigure(line_buffering=True)
main()
