#!/usr/bin/env python3
"""
Convert the official NFC Forum N-mark PNG into the A8 bitmap that
src/nfc_icon.h exposes. A8 (8-bit alpha) is the same format the old
hand-drawn icon used, so the recolour-on-style path in main.cpp's
update_nfc_indicator() — flip green when DLDO1 is enabled, grey when
off — continues to work without code changes.
"""

from PIL import Image, ImageOps

IN_PATH  = "/home/adobeflash/Downloads/NFC-Logo.png"
OUT_PATH = "/home/adobeflash/Downloads/twatch-ultra-clock/src/nfc_icon.h"
TARGET   = 26      # 26×26 — same visual weight as the old 28×22 mark
                   # in the home-screen status row, and stays cleanly
                   # square (the NFC Forum mark is square).

img = Image.open(IN_PATH).convert("RGBA")

# The source PNG has wide white padding around the logo — crop to the
# tight bounding box of the actual mark before resizing so the inscribed
# logo fills the target square rather than ending up tiny in the centre.
greyscale = ImageOps.invert(img.convert("L"))  # invert so dark logo → bright
bbox = greyscale.getbbox()
if bbox:
    img = img.crop(bbox)

# Now make the cropped image square (pad the shorter axis with the
# background colour so we don't squash the logo).
w, h = img.size
side = max(w, h)
sq = Image.new("RGBA", (side, side), (255, 255, 255, 255))
sq.paste(img, ((side - w) // 2, (side - h) // 2), img)
img = sq

img = img.resize((TARGET, TARGET), Image.LANCZOS)

# Build the A8 alpha mask: dark logo pixels → opaque (alpha = 255 - luminance),
# white background → transparent. LANCZOS resampling gives anti-aliased
# edges, so the alpha gradient at boundaries comes for free.
mask = bytearray(TARGET * TARGET)
for y in range(TARGET):
    for x in range(TARGET):
        r, g, b, a = img.getpixel((x, y))
        # Background was painted full-alpha white in `sq`; the actual
        # "intensity" of the logo at this pixel is the inverse of the
        # luminance (dark = strong).
        lum = (r * 30 + g * 59 + b * 11) // 100
        alpha = 255 - lum
        # If the source PNG had a transparent pixel, honour that.
        if a == 0:
            alpha = 0
        mask[y * TARGET + x] = alpha

# Emit nfc_icon.h with the new bitmap. Header layout matches what LVGL 9
# expects for an A8 image; the existing main.cpp inclusion and recolor
# path is untouched.
data_size = len(mask)
with open(OUT_PATH, "w") as f:
    f.write(f"""#pragma once
#include <lvgl.h>

// NFC indicator icon — 8-bit alpha bitmap of the official NFC Forum
// N-mark, generated from the canonical PNG by tools/gen_nfc_icon.py.
// Rendered as an lv_image on the clock screen and recoloured live by
// update_nfc_indicator() (green when DLDO1 is enabled, grey when off).
static const uint8_t nfc_icon_map[{data_size}] = {{
""")
    for i in range(0, data_size, 14):
        chunk = mask[i:i+14]
        f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write(f"""}};

static const lv_image_dsc_t nfc_icon_dsc = {{
    .header = {{
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_A8,
        .flags  = 0,
        .w      = {TARGET},
        .h      = {TARGET},
        .stride = {TARGET},
    }},
    .data_size = {data_size},
    .data      = nfc_icon_map,
}};
""")

print(f"wrote {OUT_PATH}: {TARGET}×{TARGET}, {data_size} B alpha data")
