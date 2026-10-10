#!/usr/bin/env python3
"""Converts an AIRSREC1 session recording, whose audio records lack RTP timestamps, to AIRSREC2.

usage: rec-add-rtp.py <old.bin> <new.bin>

AIRSREC1 audio records carry only UxPlay's sender time. This rebuilds the RTP timestamp from it: a
sender-time step within 0.3 ms of a whole number of packets advances RTP by that many packets, and any
other step is taken as UxPlay remapping RTP to sender time (its first real sync replacing the estimate
it starts a stream with), so RTP advances by one packet. Prints every remapping it finds.
"""
import struct
import sys

HEADER = struct.Struct("<IIqQII")
REC_AUDIO_FORMAT, REC_AUDIO = 4, 5
SAMPLE_RATE = 44100

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f, open(dst, "wb") as out:
    assert f.read(8) == b"AIRSREC1", "not an AIRSREC1 recording"
    out.write(b"AIRSREC2")
    spf, rtp, last_ntp, first = 480, 0, None, None
    while True:
        head = f.read(HEADER.size)
        if len(head) < HEADER.size:
            break
        kind, length, arrival, ntp, a, b = HEADER.unpack(head)
        payload = f.read(length)
        first = arrival if first is None else first
        if kind == REC_AUDIO_FORMAT:
            spf = b or (352 if a == 2 else 480)
            last_ntp = None
        elif kind == REC_AUDIO:
            if last_ntp is not None:
                packet_ns = spf * 1e9 / SAMPLE_RATE
                packets = round((ntp - last_ntp) / packet_ns)
                if packets >= 1 and abs(ntp - last_ntp - packets * packet_ns) < 0.3e6:
                    rtp += packets * spf
                else:
                    rtp += spf
                    print(f"t={(arrival - first) / 1e9:.3f} s: sender time moved {(ntp - last_ntp - packet_ns) / 1e6:+.1f} ms")
            last_ntp = ntp
            a = rtp & 0xFFFFFFFF
        out.write(HEADER.pack(kind, length, arrival, ntp, a, b))
        out.write(payload)
