#!/usr/bin/env python3
"""Simulate a glance at the real watch: is everything that matters still readable?

  python3 glance.py out.png [more.png ...]

Writes out_glance.png next to each input: the 200x228 render averaged down to
100x114 (each output pixel is a 2x2 block), then blown up 4x with hard edges so
it can be inspected.

Why half resolution: the Pebble Time 2 screen is about 25 mm wide, so a pixel
is 0.126 mm, roughly 1 arcminute at a 40 cm glance. Averaging 2x2 blocks keeps
detail of about 2 arcminutes, the equivalent of 20/40 vision or a quick glance
in poor light. If a value can't be read in the glance image, it won't be read
on the wrist either.
"""
import os
import sys

from PIL import Image


def glance(path):
    im = Image.open(path).convert('RGB')
    if im.size != (200, 228):
        raise SystemExit('%s: expected a 1x 200x228 render, got %dx%d' % ((path,) + im.size))
    small = im.reduce(2)                      # box average of each 2x2 block
    big = small.resize((small.width * 4, small.height * 4), Image.NEAREST)
    root, ext = os.path.splitext(path)
    out = root + '_glance' + ext
    big.save(out)
    return out


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for p in sys.argv[1:]:
        print(glance(p))
