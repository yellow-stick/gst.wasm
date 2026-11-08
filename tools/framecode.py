#!/usr/bin/env python3
"""Decode the frame-number strips burned by tools/make_test_clips.sh.

A strip is 16 cells of 20 px: cell 0 is the white reference, cells 1..12
are bits 0..11. Clip A's strip is rows 0..31, clip B's rows 208..239.
During a cross-fade a strip is scaled by that clip's alpha; bits are read
relative to the reference cell, so each clip stays decodable while its
alpha is high enough.

Usage:
  framecode.py IMAGE...      decode PNG captures (320x240 canvas captures)
  framecode.py --video FILE  decode every frame of a video with ffmpeg
"""
import json
import subprocess
import sys

W, H = 320, 240
STRIPS = {'A': (4, 28), 'B': (212, 236)}  # sample rows inside each strip
MIN_REF = 40  # reference luma below this: the strip is absent


def luma(px, x, y):
    i = (y * W + x) * 3
    return (px[i] * 299 + px[i + 1] * 587 + px[i + 2] * 114) // 1000


def cell_mean(px, cell, y0, y1):
    xs = range(cell * 20 + 5, cell * 20 + 15)
    vals = [luma(px, x, y) for y in range(y0, y1, 4) for x in xs]
    return sum(vals) / len(vals)


def decode_rgb(px):
    """px: bytes of W*H*3 RGB. Returns {'A': {...}|None, 'B': {...}|None}."""
    out = {}
    for name, (y0, y1) in STRIPS.items():
        ref = cell_mean(px, 0, y0, y1)
        if ref < MIN_REF:
            out[name] = None
            continue
        n = 0
        for bit in range(12):
            if cell_mean(px, bit + 1, y0, y1) > ref / 2:
                n |= 1 << bit
        out[name] = {'frame': n, 'alpha': round(ref / 255, 3)}
    return out


def decode_png(path):
    from PIL import Image
    img = Image.open(path).convert('RGB')
    if img.size != (W, H):
        img = img.resize((W, H), Image.NEAREST)
    return decode_rgb(img.tobytes())


def decode_video(path):
    raw = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
        check=True, capture_output=True).stdout
    size = W * H * 3
    return [decode_rgb(raw[i:i + size]) for i in range(0, len(raw), size)]


if __name__ == '__main__':
    if sys.argv[1:2] == ['--video']:
        print(json.dumps(decode_video(sys.argv[2])))
    else:
        print(json.dumps({p: decode_png(p) for p in sys.argv[1:]}, indent=1))
