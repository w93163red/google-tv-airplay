#!/usr/bin/env python3
"""Writes the video access units of a session recording as an Annex-B elementary stream.

usage: rec-extract.py <rec.bin> <out.h264|out.h265>
Prints the record counts and the time span so a recording can be sanity-checked before replay.
"""
import struct
import sys

HEADER = struct.Struct("<IIqQII")

src, dst = sys.argv[1], sys.argv[2]
counts = {}
first = last = None
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
names = {1: "codec", 2: "video", 3: "video_reset", 4: "audio_format", 5: "audio", 6: "audio_flush"}
print({names.get(k, k): v for k, v in sorted(counts.items())}, f"span {(last - first) / 1e9:.1f} s")
