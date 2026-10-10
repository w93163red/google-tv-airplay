#!/usr/bin/env python3
"""Prints the smart-zoom decisions the app would make over a session recording, sampled every 500 ms.

usage: zoom-timeline.py <rec.bin> <work-dir>
Decodes every video frame to a 96 px wide RGB image in <work-dir> with ffmpeg, then runs RecordingTimelineTest
over them. The frames show the mirrored screen, so keep <work-dir> outside the repo.
"""
import os
import pathlib
import struct
import subprocess
import sys

HEADER = struct.Struct("<IIqQII")
REC_VIDEO_CODEC, REC_VIDEO = 1, 2

src, work = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
work.mkdir(parents=True, exist_ok=True)
codec = "h264"
units = []
arrivals = []
with open(src, "rb") as f:
    assert f.read(8) == b"AIRSREC1", "not a session recording"
    while head := f.read(HEADER.size):
        kind, length, arrival, _ntp, a, _b = HEADER.unpack(head)
        payload = f.read(length)
        if kind == REC_VIDEO_CODEC:
            codec = "hevc" if a == 2 else "h264"
        elif kind == REC_VIDEO:
            units.append(payload)
            arrivals.append(arrival)
(work / "times.txt").write_text("".join(f"{(t - arrivals[0]) / 1e9:.3f}\n" for t in arrivals))


def write_stream(path, first, last):
    with open(path, "wb") as out:
        for unit in units[first:last + 1]:
            out.write(unit)


stream = work / "video.es"
write_stream(stream, 0, len(units) - 1)
sizes = subprocess.run(
    ["ffprobe", "-v", "error", "-f", codec, "-show_entries", "frame=width,height", "-of", "csv=p=0", str(stream)],
    check=True, capture_output=True, text=True,
).stdout.split()
assert len(sizes) == len(units), f"{len(sizes)} decoded frames for {len(units)} access units"
# ffmpeg sizes all its output by the first frame, so each run of one stream size is decoded from its own stream.
# A size change arrives with new parameter sets and a key frame, so a run decodes without the units before it.
start = 0
while start < len(sizes):
    end = start
    while end + 1 < len(sizes) and sizes[end + 1] == sizes[start]:
        end += 1
    write_stream(stream, start, end)
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-f", codec, "-i", str(stream), "-fps_mode", "passthrough",
         "-vf", "scale=96:-1:flags=area,format=rgb24", "-start_number", str(start + 1), str(work / "%05d.ppm")],
        check=True,
    )
    assert (work / f"{end + 1:05d}.ppm").exists(), f"frames {start + 1}-{end + 1} did not all decode"
    print(f"frames {start + 1}-{end + 1}: {sizes[start]}")
    start = end + 1
stream.unlink()

repo = pathlib.Path(__file__).resolve().parent.parent
subprocess.run(
    ["./gradlew", "-q", "testDebugUnitTest", "--tests", "dev.airscreen.RecordingTimelineTest", "--rerun"],
    cwd=repo, env=os.environ | {"AIRSCREEN_FRAMES": str(work)}, check=True,
)
print((work / "timeline.txt").read_text(), end="")
