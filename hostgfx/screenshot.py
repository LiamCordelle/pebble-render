"""Turn a raw Emery framebuffer into what `pebble screenshot` saves.

Mirrors pebble-tool 5.0.40 (pebble_tool/commands/screenshot.py) and libpebble2's
screenshot decoder:
  1. libpebble2 Screenshot._decode_8bit: each GColor8 byte (0bAARRGGBB) becomes
     R,G,B = ((p >> 4) & 3) * 85, ((p >> 2) & 3) * 85, (p & 3) * 85. Alpha bits ignored.
  2. ScreenshotCommand._correct_colours: a fixed 64-entry lookup table approximating the
     reflective LCD (below, copied verbatim).
  3. ScreenshotCommand._roundify: adds alpha 255; only round platforms (chalk, gabbro) get
     transparent corners, so Emery is fully opaque.
  4. png.from_array(image, mode='RGBA;8'): an 8-bit RGBA PNG.
"""
from PIL import Image

WIDTH, HEIGHT = 200, 228

CORRECTION = {
    (0, 0, 0): (0, 0, 0),
    (0, 0, 85): (0, 30, 65),
    (0, 0, 170): (0, 67, 135),
    (0, 0, 255): (0, 104, 202),
    (0, 85, 0): (43, 74, 44),
    (0, 85, 85): (39, 81, 79),
    (0, 85, 170): (22, 99, 141),
    (0, 85, 255): (0, 125, 206),
    (0, 170, 0): (94, 152, 96),
    (0, 170, 85): (92, 155, 114),
    (0, 170, 170): (87, 165, 162),
    (0, 170, 255): (76, 180, 219),
    (0, 255, 0): (142, 227, 145),
    (0, 255, 85): (142, 230, 158),
    (0, 255, 170): (138, 235, 192),
    (0, 255, 255): (132, 245, 241),
    (85, 0, 0): (74, 22, 27),
    (85, 0, 85): (72, 39, 72),
    (85, 0, 170): (64, 72, 138),
    (85, 0, 255): (47, 107, 204),
    (85, 85, 0): (86, 78, 54),
    (85, 85, 85): (84, 84, 84),
    (85, 85, 170): (79, 103, 144),
    (85, 85, 255): (65, 128, 208),
    (85, 170, 0): (117, 154, 100),
    (85, 170, 85): (117, 157, 118),
    (85, 170, 170): (113, 166, 164),
    (85, 170, 255): (105, 181, 221),
    (85, 255, 0): (158, 229, 148),
    (85, 255, 85): (157, 231, 160),
    (85, 255, 170): (155, 236, 194),
    (85, 255, 255): (149, 246, 242),
    (170, 0, 0): (153, 53, 63),
    (170, 0, 85): (152, 62, 90),
    (170, 0, 170): (149, 86, 148),
    (170, 0, 255): (143, 116, 210),
    (170, 85, 0): (157, 91, 77),
    (170, 85, 85): (157, 96, 100),
    (170, 85, 170): (154, 112, 153),
    (170, 85, 255): (149, 135, 213),
    (170, 170, 0): (175, 160, 114),
    (170, 170, 85): (174, 163, 130),
    (170, 170, 170): (171, 171, 171),
    (170, 170, 255): (167, 186, 226),
    (170, 255, 0): (201, 232, 157),
    (170, 255, 85): (201, 234, 167),
    (170, 255, 170): (199, 240, 200),
    (170, 255, 255): (195, 249, 247),
    (255, 0, 0): (227, 84, 98),
    (255, 0, 85): (226, 88, 116),
    (255, 0, 170): (225, 106, 163),
    (255, 0, 255): (222, 131, 220),
    (255, 85, 0): (230, 110, 107),
    (255, 85, 85): (230, 114, 124),
    (255, 85, 170): (227, 127, 167),
    (255, 85, 255): (225, 148, 223),
    (255, 170, 0): (241, 170, 134),
    (255, 170, 85): (241, 173, 147),
    (255, 170, 170): (239, 181, 184),
    (255, 170, 255): (236, 195, 235),
    (255, 255, 0): (255, 238, 171),
    (255, 255, 85): (255, 241, 181),
    (255, 255, 170): (255, 246, 211),
    (255, 255, 255): (255, 255, 255),
}


def decode_8bit(p):
    return (((p >> 4) & 3) * 85, ((p >> 2) & 3) * 85, (p & 3) * 85)


# 256-entry lookup: framebuffer byte -> corrected RGBA.
BYTE_TO_RGBA = [CORRECTION[decode_8bit(p)] + (255,) for p in range(256)]
BYTE_TO_RGBA_RAW = [decode_8bit(p) + (255,) for p in range(256)]


def framebuffer_to_image(raw, correct=True):
    if len(raw) != WIDTH * HEIGHT:
        raise ValueError('expected %d framebuffer bytes, got %d' % (WIDTH * HEIGHT, len(raw)))
    lut = BYTE_TO_RGBA if correct else BYTE_TO_RGBA_RAW
    data = bytearray()
    for p in raw:
        data.extend(lut[p])
    return Image.frombytes('RGBA', (WIDTH, HEIGHT), bytes(data))

