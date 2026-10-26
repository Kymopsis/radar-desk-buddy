# -*- coding: utf-8 -*-
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

SIZE = 32
OUT = Path(__file__).resolve().parents[1] / "components/ui/fonts/ui_font_32_digits.c"
HDR = Path(__file__).resolve().parents[1] / "components/ui/include/ui_font_32_digits.h"
CHARS = "0123456789:"
# Fredoka (SIL Open Font License 1.1): rounded digits for the clock.
# Download Fredoka[wdth,wght].ttf from https://github.com/google/fonts/tree/main/ofl/fredoka
# into tools/fonts/Fredoka.ttf (not committed), or point UI_DIGIT_FONT at another OFL font.
import os
FONT_CAND = [
    p for p in (
        os.environ.get("UI_DIGIT_FONT", ""),
        str(Path(__file__).resolve().parent / "fonts" / "Fredoka.ttf"),
    ) if p
]

def load():
    for p in FONT_CAND:
        if Path(p).exists():
            try:
                return ImageFont.truetype(p, SIZE, index=0)
            except Exception:
                try:
                    return ImageFont.truetype(p, SIZE)
                except Exception:
                    pass
    raise SystemExit("no digit font")

font = load()
glyphs = []
for ch in CHARS:
    bbox = font.getbbox(ch)
    w = max(bbox[2] - bbox[0], 1)
    h = max(bbox[3] - bbox[1], 1)
    canvas_w = 28 if ch != ":" else 14
    img = Image.new("L", (canvas_w, SIZE), 0)
    draw = ImageDraw.Draw(img)
    x = (canvas_w - w) // 2 - bbox[0]
    y = (SIZE - h) // 2 - bbox[1]
    draw.text((x, y), ch, font=font, fill=255)
    nbytes = (canvas_w * SIZE + 7) // 8
    bits = bytearray(nbytes)
    for py in range(SIZE):
        for px in range(canvas_w):
            if img.getpixel((px, py)) > 100:
                i = py * canvas_w + px
                bits[i // 8] |= 1 << (7 - (i % 8))
    glyphs.append((ch, canvas_w, bits, nbytes))

max_bytes = max(g[3] for g in glyphs)
lines = [
    '/* Generated from Fredoka (SIL Open Font License 1.1, see OFL.txt in this folder). */',
    '#include "ui_font_32_digits.h"',
    "#include <stddef.h>",
    "#include <stdint.h>",
    f"#define N {len(glyphs)}",
    f"#define H {SIZE}",
    f"#define MAXB {max_bytes}",
    "typedef struct { uint32_t cp; uint8_t w; uint8_t bits[MAXB]; } g_t;",
    "static const g_t s_g[N] = {",
]
for ch, w, bits, nb in glyphs:
    padded = list(bits) + [0] * (max_bytes - nb)
    hexb = ", ".join(f"0x{b:02X}" for b in padded)
    lines.append(f"  {{ {ord(ch)}, {w}, {{ {hexb} }} }},")
lines += [
    "};",
    "const uint8_t *ui_font32_bits(uint32_t cp, int *out_w)",
    "{",
    "  for (int i = 0; i < N; ++i) {",
    "    if (s_g[i].cp == cp) { if (out_w) *out_w = s_g[i].w; return s_g[i].bits; }",
    "  }",
    "  return NULL;",
    "}",
    f"int ui_font32_height(void) {{ return {SIZE}; }}",
]
OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
HDR.write_text(
    """#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
const uint8_t *ui_font32_bits(uint32_t cp, int *out_w);
int ui_font32_height(void);
#ifdef __cplusplus
}
#endif
""",
    encoding="utf-8",
)
print("digits ok", OUT)
