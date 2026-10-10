#!/usr/bin/env python3
"""Summarizes the once-per-second AirScreenStats lines of a replay log.

Per metric: the median of the per-second p50 values, and the median and maximum of the per-second
p99 values. A window holds about one second of samples, so its p99 is close to its maximum and the
maximum over windows bounds the whole-run p99 from above. The first --skip windows with video are
left out as startup. The audio part repeats the video rows for the windows that measured an A/V
offset, after their first --skip, so the switch to audio-timed presentation is left out too.

usage: replay-summary.py LOG [--skip 3] [--windows]
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


def show(label, rows, key50, key99):
    p50 = [r[key50] for r in rows if key50 in r]
    p99 = [r[key99] for r in rows if key99 in r]
    if p50:
        print(f"{label:<16} p50 median {statistics.median(p50):7.1f}   p99 median {statistics.median(p99):7.1f}"
              f"   p99 max {max(p99):7.1f}   windows {len(p50)}")


def show_video(video):
    total = lambda k: int(sum(w.get(k, 0) for w in video))
    gap = max(abs(w["rendered"] - w["in"]) for w in video)
    pushed = f"pushed {total('pushed')} " if "pushed" in video[0] else ""
    overflow = f" overflow {total('overflow')}" if "overflow" in video[0] else ""
    print(f"video windows {len(video)}: {pushed}in {total('in')} rendered {total('rendered')} dropped {total('dropped')}"
          f"{overflow}; per-second in median {statistics.median(w['in'] for w in video):.0f}"
          f" min {min(w['in'] for w in video):.0f} max {max(w['in'] for w in video):.0f}"
          f" rendered median {statistics.median(w['rendered'] for w in video):.0f}"
          f" rendered min {min(w['rendered'] for w in video):.0f} max |rendered-in| {gap:.0f}")
    show("push_ms", video, "push_ms p50", "push_ms p99")
    show("decode_ms", video, "decode_ms p50", "decode_ms p99")
    show("glass_ms", video, "glass_ms p50", "glass_ms p99")
    av = [w for w in video if w.get("av_offset_ms n", 0) > 0]
    show("av_offset_ms", av, "av_offset_ms p50", "av_offset_ms abs_p99")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=int, default=3)
    ap.add_argument("--windows", action="store_true", help="also print every video window")
    args = ap.parse_args()
    video, audio = [], []
    for line in open(args.log, errors="replace"):
        if "AirScreenStats" in line:
            body = line.split("AirScreenStats", 1)[1].split(":", 1)[1]
            fields = parse(body)
            if "audio xruns" in line:
                audio.append(fields)
            else:
                video.append(fields)
                if args.windows:
                    print(body.strip())
        elif "AirScreen" in line and ("decoder" in line or "AAudio" in line or "replay" in line or "overflow" in line):
            print(line.rstrip())
    everything = [w for w in video if w.get("in", 0) > 0 or w.get("pushed", 0) > 0]
    if everything and "pushed" in everything[0]:
        print(f"all video windows {len(everything)}: pushed {int(sum(w['pushed'] for w in everything))}")
    video = everything[args.skip:]
    audio = audio[args.skip:]
    if video:
        show_video(video)
    with_audio = [w for w in video if w.get("av_offset_ms n", 0) > 0][args.skip:]
    if with_audio:
        print("-- audio part --")
        show_video(with_audio)
    if audio:
        print(f"audio windows {len(audio)}: xruns {int(sum(w['audio xruns'] for w in audio))}"
              f" dropped_frames {int(sum(w['audio dropped_frames'] for w in audio))}"
              f" stale {int(sum(w.get('audio stale', 0) for w in audio))}"
              f" level_ms median {statistics.median(w['audio level_ms'] for w in audio):.0f}")
        show("out_latency_ms", audio, "out_latency_ms p50", "out_latency_ms p99")


if __name__ == "__main__":
    main()
