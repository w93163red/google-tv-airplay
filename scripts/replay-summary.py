#!/usr/bin/env python3
"""Summarizes the once-per-second AirScreenStats lines of a replay log.

Per metric: the median of the per-second p50 values, and the median and maximum of the per-second
p99 values. A window holds about one second of samples, so its p99 is close to its maximum and the
maximum over windows bounds the whole-run p99 from above. The first --skip windows with video are
left out as startup.

usage: replay-summary.py LOG [--skip 3]
"""
import argparse
import statistics


def parse(text):
    """'audio xruns=1 decode_ms p50=2.0' -> {'audio xruns': 1.0, 'decode_ms p50': 2.0}: a bare word names a group."""
    fields, group = {}, ""
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[f"{group} {key}".strip()] = float(value)
        else:
            group = token
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=int, default=3)
    args = ap.parse_args()
    video, audio = [], []
    for line in open(args.log, errors="replace"):
        if "AirScreenStats" in line:
            fields = parse(line.split("AirScreenStats", 1)[1].split(":", 1)[1])
            (audio if "audio xruns" in line else video).append(fields)
        elif "AirScreen" in line and ("decoder" in line or "AAudio" in line or "replay" in line):
            print(line.rstrip())
    video = [w for w in video if w.get("in", 0) > 0][args.skip:]
    audio = audio[args.skip:]

    def show(label, rows, key50, key99):
        p50 = [r[key50] for r in rows if key50 in r]
        p99 = [r[key99] for r in rows if key99 in r]
        if p50:
            print(f"{label:<16} p50 median {statistics.median(p50):7.1f}   p99 median {statistics.median(p99):7.1f}"
                  f"   p99 max {max(p99):7.1f}   windows {len(p50)}")

    if video:
        total = lambda k: int(sum(w.get(k, 0) for w in video))
        print(f"video windows {len(video)}: in {total('in')} rendered {total('rendered')} dropped {total('dropped')};"
              f" per-second in median {statistics.median(w['in'] for w in video):.0f}"
              f" rendered median {statistics.median(w['rendered'] for w in video):.0f}"
              f" rendered min {min(w['rendered'] for w in video):.0f}")
        show("decode_ms", video, "decode_ms p50", "decode_ms p99")
        show("glass_ms", video, "glass_ms p50", "glass_ms p99")
        av = [w for w in video if w.get("av_offset_ms n", 0) > 0]
        show("av_offset_ms", av, "av_offset_ms p50", "av_offset_ms abs_p99")
    if audio:
        print(f"audio windows {len(audio)}: xruns {int(sum(w['audio xruns'] for w in audio))}"
              f" dropped_frames {int(sum(w['audio dropped_frames'] for w in audio))}"
              f" level_ms median {statistics.median(w['audio level_ms'] for w in audio):.0f}")
        show("out_latency_ms", audio, "out_latency_ms p50", "out_latency_ms p99")


if __name__ == "__main__":
    main()
