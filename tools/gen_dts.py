#!/usr/bin/env python3
"""Generate src/dts_clips.c: discrete 5.1 test clips encoded as DTS inside IEC 61937 (S/PDIF) bursts.

Each clip is exactly 4 s (375 DTS frames of 512 samples) so it loops seamlessly on the IOP: a spoken label on
the channel under test (same Piper voice as the other tests), then pink noise bursts on that channel only.
The output is 48 kHz stereo s16 words, ready to be streamed through SPU2 core 0 in bitstream mode.

Needs ffmpeg with the (experimental) native DTS encoder and the spdif muxer.
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_voices  # noqa: E402

RATE = 48000
FRAMES = 512 * 375  # 4.0 s
# 5.1(side) channel order used for the ffmpeg input: FL FR FC LFE SL SR
FL, FR, FC, LFE, SL, SR = range(6)

CLIPS = [
    ("DTS, front left", [FL], "noise"),
    ("DTS, center", [FC], "noise"),
    ("DTS, front right", [FR], "noise"),
    ("DTS, surround right", [SR], "noise"),
    ("DTS, surround left", [SL], "noise"),
    ("DTS, subwoofer", [LFE], "lfe"),
    ("DTS, dialogue and surround left", [SL], "dual"),
]


def pink(n, seed):
    rnd = random.Random(seed)
    b = [0.0] * 7
    out = []
    for _ in range(n):
        w = rnd.gauss(0, 0.2)
        b[0] = 0.99886 * b[0] + w * 0.0555179
        b[1] = 0.99332 * b[1] + w * 0.0750759
        b[2] = 0.96900 * b[2] + w * 0.1538520
        b[3] = 0.86650 * b[3] + w * 0.3104856
        b[4] = 0.55000 * b[4] + w * 0.5329522
        b[5] = -0.7616 * b[5] - w * 0.0168980
        out.append((b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362) * 0.25)
        b[6] = w * 0.115926
    return out


def highpass(x, fc=150.0, passes=2):
    # 2nd-order Butterworth high-pass, applied twice (4th order). Keeps the test noise out of the LFE band:
    # FFmpeg's experimental DTS encoder otherwise leaks surround-right bass into the LFE channel.
    w = 2 * math.pi * fc / RATE
    cw, alpha = math.cos(w), math.sin(w) / (2 * 0.70710678)
    a0 = 1 + alpha
    b0, b1, b2 = (1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0
    a1, a2 = -2 * cw / a0, (1 - alpha) / a0
    for _ in range(passes):
        y = []
        z1 = z2 = 0.0
        for v in x:
            o = b0 * v + z1
            z1 = b1 * v - a1 * o + z2
            z2 = b2 * v - a2 * o
            y.append(o)
        x = y
    return x


def fade(i, n, f=480):
    return min(1.0, i / f, (n - i) / f)


def build_clip(label, chans, kind, tmp, idx):
    pcm = [[0.0] * FRAMES for _ in range(6)]
    voice = [v / 32768.0 * 0.7 for v in gen_voices.speak(label, tmp)] if os.environ.get("PIPER_MODEL") else gen_voices.tone(idx)
    if not os.environ.get("PIPER_MODEL"):
        voice = [v / 32768.0 * 0.7 for v in voice]
    start = int(0.1 * RATE)
    # spoken label from the channel under test (the subwoofer test announces from the centre)
    vch = FC if kind in ("lfe", "dual") else chans[0]
    for i, v in enumerate(voice[: FRAMES - start - RATE]):
        pcm[vch][start + i] = v
    burst_start = start + len(voice) + int(0.3 * RATE)
    noise = highpass(pink(FRAMES, 11 + idx))
    t = burst_start
    while t + int(0.6 * RATE) < FRAMES - 480:
        n = int(0.6 * RATE)
        for i in range(n):
            g = fade(i, n)
            if kind == "lfe":
                s = 0.35 * math.sin(2 * math.pi * 40 * (t + i) / RATE) * g
            else:
                s = noise[t + i] * g
            for c in chans:
                pcm[c][t + i] += s
        t += RATE  # 0.6 s on, 0.4 s off
    if kind == "dual":
        # looped dialogue in the centre under the surround bursts
        line = gen_voices.speak(gen_voices.LABELS[-1], tmp) if os.environ.get("PIPER_MODEL") else gen_voices.tone(3)
        p = burst_start
        while p < FRAMES - 480:
            for i, v in enumerate(line):
                if p + i >= FRAMES - 480:
                    break
                pcm[FC][p + i] += v / 32768.0 * 0.5
            p += len(line) + int(0.4 * RATE)

    raw = os.path.join(tmp, "clip.raw")
    with open(raw, "wb") as f:
        for i in range(FRAMES):
            f.write(struct.pack("<6h", *[max(-32768, min(32767, int(pcm[c][i] * 32767))) for c in range(6)]))
    out = os.path.join(tmp, "clip.spdif")
    base = ["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(RATE), "-ac", "6"]
    enc = ["-i", raw, "-c:a", "dca", "-strict", "-2", "-b:a", "1509k", "-ar", str(RATE), "-f", "spdif", out]
    try:
        subprocess.run(base + ["-ch_layout", "5.1(side)"] + enc, check=True)
    except subprocess.CalledProcessError:
        subprocess.run(base + ["-channel_layout", "5.1(side)"] + enc, check=True)
    data = open(out, "rb").read()
    words = list(struct.unpack("<%dh" % (len(data) // 2), data))
    want = FRAMES * 2
    words = (words + [0] * want)[:want]  # exactly 4 s of S/PDIF frames
    return words


def main(out_path):
    with tempfile.TemporaryDirectory() as tmp, open(out_path, "w") as out:
        out.write("// Generated by tools/gen_dts.py - do not edit\n#include \"dts.h\"\n\n")
        for idx, (label, chans, kind) in enumerate(CLIPS):
            w = build_clip(label, chans, kind, tmp, idx)
            out.write("static const short dts_clip_%d[%d] __attribute__((aligned(64))) = {\n" % (idx, len(w)))
            for k in range(0, len(w), 24):
                out.write("\t" + ",".join(str(v) for v in w[k:k + 24]) + ",\n")
            out.write("};\n\n")
            print("dts clip %d (%s): %d frames" % (idx, label, len(w) // 2), file=sys.stderr)
        out.write("const struct DtsClip g_dts_clips[%d] = {\n" % len(CLIPS))
        for idx in range(len(CLIPS)):
            out.write("\t{dts_clip_%d, %d},\n" % (idx, FRAMES))
        out.write("};\nconst int g_num_dts_clips = %d;\n" % len(CLIPS))


if __name__ == "__main__":
    main(sys.argv[1])
