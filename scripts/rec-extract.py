#!/usr/bin/env python3
"""Writes the video access units of a session recording as an Annex-B elementary stream.

usage: rec-extract.py <rec.bin> <out.h264|out.h265>
Prints the record counts, the time span and the video access units per second so a recording can be sanity-checked before replay.
"""
import statistics
import struct
import sys

HEADER = struct.Struct("<IIqQII")

src, dst = sys.argv[1], sys.argv[2]
counts = {}
first = last = None
video_arrivals = []
with open(src, "rb") as f, open(dst, "wb") as out:
    assert f.read(8) == b"AIRSREC1", "not a session recording"
    while True:
        head = f.read(HEADER.size)
        if len(head) < HEADER.size:
            break
        kind, length, arrival, _ntp, _a, _b = HEADER.unpack(head)
        payload = f.read(length)
        counts[kind] = counts.get(kind, 0) + 1
        first = arrival if first is None else first
        last = arrival
        if kind == 2:
            out.write(payload)
            video_arrivals.append(arrival)
names = {1: "codec", 2: "video", 3: "video_reset", 4: "audio_format", 5: "audio", 6: "audio_flush"}
print({names.get(k, k): v for k, v in sorted(counts.items())}, f"span {(last - first) / 1e9:.1f} s")
if video_arrivals:
    per_second = {}
    for arrival in video_arrivals:
        second = (arrival - video_arrivals[0]) // 1_000_000_000
        per_second[second] = per_second.get(second, 0) + 1
    full = [per_second.get(s, 0) for s in range(max(per_second))]
    if full:
        print(f"video per second (full seconds): median {statistics.median(full):.0f} min {min(full)} max {max(full)}")
