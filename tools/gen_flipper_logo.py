#!/usr/bin/env python3
"""
Convert the Flipper Zero logo PNG into a 1-bit LVGL image (A1 colour format).

LVGL renders A1 images as an alpha mask coloured by the style's
`image_recolor` — so embedding it as 1 bit per pixel keeps the flash
cost tiny (~1.2 KB at 120×80) and lets the icon inherit a single
"Flipper orange" tint at draw time.

Output: src/flipper_logo_img.c with a const lv_image_dsc_t named
`flipper_logo_img` ready to pass to lv_image_set_src().
"""

from PIL import Image

IN_PATH    = "/home/adobeflash/Downloads/flipper_logo.png"
OUT_PATH   = "/home/adobeflash/Downloads/twatch-ultra-clock/src/flipper_logo_img.c"
TARGET_W   = 120                       # tile is 180×180; leave room for label
THRESHOLD  = 128                       # pixels with luminance < this → opaque

# LVGL 9 constants we need to bake in by value (header is bit-packed so
# we emit the raw uint32 rather than constructing a struct expression):
COLOR_FORMAT_A1   = 0x0B
HEADER_MAGIC      = 0x19

# ---- load + resize -------------------------------------------------------

img = Image.open(IN_PATH).convert("RGBA")
ow, oh = img.size
tw = TARGET_W
th = int(round(oh * TARGET_W / ow))
img = img.resize((tw, th), Image.LANCZOS)

# ---- threshold to 1-bit alpha mask --------------------------------------
#
# The source PNG is orange-on-white with no real alpha channel; we treat
# any pixel whose luminance is below the threshold (the dark orange
# strokes) as opaque mask=1, everything else as transparent mask=0.

stride_bits  = tw                      # one row of bits
stride_bytes = (stride_bits + 7) // 8  # padded to whole bytes
data = bytearray(stride_bytes * th)

for y in range(th):
    for x in range(tw):
        r, g, b, a = img.getpixel((x, y))
        # luminance — orange (R≈255, G≈130, B≈0) ≈ 175 in Y′ space, white
        # (255,255,255) ≈ 255; using 200 as cutoff keeps the orange strokes
        # mask=1 and the white background mask=0 cleanly.
        lum = (r * 30 + g * 59 + b * 11) // 100
        if a > 128 and lum < 200:
            byte_idx = y * stride_bytes + x // 8
            bit      = 7 - (x % 8)     # MSB-first
            data[byte_idx] |= (1 << bit)

# ---- emit C ---------------------------------------------------------------
#
# We hand-pack the lv_image_header_t into a uint32_t because the struct is
# defined as bit-fields and a designated initializer would be sensitive to
# little-/big-endian field ordering. The layout below matches the
# little-endian variant LVGL ships for ESP32 builds.

# header.magic (8 bits) | cf (8 bits) | flags (16 bits) — packed LE.
hdr0 = (HEADER_MAGIC & 0xFF) | ((COLOR_FORMAT_A1 & 0xFF) << 8)  # flags = 0
# Next 32 bits: w (16) | h (16)
hdr1 = (tw & 0xFFFF) | ((th & 0xFFFF) << 16)
# Next 32 bits: stride (16) | reserved_2 (16)
hdr2 = (stride_bytes & 0xFFFF)

data_size = len(data)

with open(OUT_PATH, "w") as f:
    f.write(f"""/*
 * Flipper Zero logo — 1-bit alpha mask, {tw}×{th} px, generated from
 * the canonical PNG via tools/gen_flipper_logo.py. Recoloured at draw
 * time to the brand orange (#FF8200) via the image widget's style.
 *
 * Flash footprint: header (~24 B) + {data_size} B pixel data = ~{(data_size + 24) / 1024:.1f} KB.
 *
 * The lv_image_header_t is hand-packed into bit-field-equivalent
 * uint32_t words because designated-initialiser order is sensitive to
 * the platform's endianness; this layout matches LVGL's little-endian
 * struct for ESP32 builds.
 */

#include "lvgl.h"

static const LV_ATTRIBUTE_LARGE_CONST uint8_t FLIPPER_LOGO_DATA[] = {{
""")
    for i in range(0, data_size, 12):
        chunk = data[i:i+12]
        f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write(f"""}};

const lv_image_dsc_t flipper_logo_img = {{
    .header = {{
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_A1,
        .flags  = 0,
        .w      = {tw},
        .h      = {th},
        .stride = {stride_bytes},
        .reserved_2 = 0,
    }},
    .data_size = {data_size},
    .data      = FLIPPER_LOGO_DATA,
    .reserved   = NULL,
    .reserved_2 = NULL,
}};
""")

print(f"wrote {OUT_PATH}: {tw}×{th}, {data_size} B pixel data")
