# -*- coding: utf-8 -*-
"""Build cute 16px CJK/Latin/Cyrillic/Hangul glyph atlas from all UI strings."""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "components/ui/fonts/ui_font_16.c"
HDR = Path(__file__).resolve().parents[1] / "components/ui/include/ui_font_16.h"
SIZE = 16

# Prefer softer UI fonts
# Noto Sans CJK (SIL Open Font License 1.1) covers zh/ja/ko/Latin/Cyrillic in one file.
# Download NotoSansCJKsc-Regular.otf from https://github.com/notofonts/noto-cjk into
# tools/fonts/ (not committed), or point UI_FONT at any OFL-licensed font.
import os
CANDIDATES = [
    p for p in (
        os.environ.get("UI_FONT", ""),
        str(Path(__file__).resolve().parent / "fonts" / "NotoSansCJKsc-Regular.otf"),
    ) if p
]

STRINGS = [
    # digits / punct
    "0123456789:./- ℃°%+ ",
    # full printable ASCII: Wi-Fi names, passwords and Telegram text can use any of it
    "".join(chr(c) for c in range(0x20, 0x7F)),
    # zh
    "有新留言向上摸摸头再摸一下关掉对端在呀不离线给TA发消息云端也能玩配网扫码连接选语言填绑定码留言今天也想你雨晴阴想你啦语言性别男版蓝色女版粉色雷达未连接服务器未连接",
    # zh-tw extras
    "頭關掉對離線給發雲網掃碼連接選語言綁定性別藍色雷達未連接伺服器未連線",
    # en + latin langs (ascii covered) accents avoided by ascii-ish strings
    "New message Touch the head to close Peer is here away offline Message them Play in cloud Setup Scan join Language bind code Radar offline Server offline",
    "Mensaje nuevo Toca la cabeza para cerrar Pareja aqui lejos offline Enviar Jugar nube Configurar Escanea unir Idioma codigo Servidor",
    "Nouveau message Touche la tete pour fermer Partenaire ici loin Hors ligne Envoyer Jouer au Config Scanner joindre Langue Serveur",
    "Neue Nachricht Kopf beruehren Tippen zum Schliessen Partner hier weg offline senden In Cloud spielen Scannen Joinen Sprache Code Server",
    "Nova mensagem Toque a cabeca para fechar Par longe Brincar na nuvem Configurar Escaneie entrar Idioma Servidor",
    # ja
    "新しい伝言頭をタッチでもう一度閉じる相手はいるよいないオフラインメッセージ送るクラウドで遊ぶ接続設定スキャンして言語とバインドサーバー未接続",
    # ko
    "새 메시지머리를 터치다시 해 닫기상대 있어요없어요오프라인보내기클라우드로 놀기설정스캔하여 연결언어바인드서버 미연결",
    # ru
    "Новое сообщение Коснись головы чтобы закрыть Пара рядом нет офлайн Написать Играть в облаке Настройка Сканируй для входа Язык код Сервер",
    # lang names shown in UI
    "简体中文繁體English日本語한국어EspanolFrancaisDeutschPortuguesРусский",
]

def load_fonts():
    fonts = []
    for path in CANDIDATES:
        p = Path(path)
        if not p.exists():
            continue
        try:
            fonts.append(ImageFont.truetype(str(p), SIZE, index=0))
        except Exception:
            try:
                fonts.append(ImageFont.truetype(str(p), SIZE))
            except Exception:
                pass
    if not fonts:
        raise SystemExit("no font")
    return fonts

fonts = load_fonts()

seen = set()
uniq = []
for blob in STRINGS:
    for ch in blob:
        if ch not in seen:
            seen.add(ch)
            uniq.append(ch)

def render_glyph(ch):
    for font in fonts:
        bbox = font.getbbox(ch)
        if bbox is None:
            continue
        w = max(bbox[2] - bbox[0], 1)
        h = max(bbox[3] - bbox[1], 1)
        # Skip if font rendered almost empty (missing glyph often thin box)
        img = Image.new("L", (SIZE, SIZE), 0)
        draw = ImageDraw.Draw(img)
        x = (SIZE - w) // 2 - bbox[0]
        y = (SIZE - h) // 2 - bbox[1]
        draw.text((x, y), ch, font=font, fill=255)
        ink = sum(1 for px in img.getdata() if px > 110)
        if ink < 8 and ord(ch) > 127:
            continue
        bits = bytearray((SIZE * SIZE + 7) // 8)
        for py in range(SIZE):
            for px in range(SIZE):
                if img.getpixel((px, py)) > 110:
                    i = py * SIZE + px
                    bits[i // 8] |= 1 << (7 - (i % 8))
        return bits
    # fallback empty
    return bytearray((SIZE * SIZE + 7) // 8)

glyphs = []
for ch in uniq:
    glyphs.append((ch, render_glyph(ch)))

lines = [
    '/* Generated from Noto Sans CJK SC (SIL Open Font License 1.1, see OFL.txt in this folder). */',
    '#include "ui_font_16.h"',
    "#include <stddef.h>",
    "#include <stdint.h>",
    "",
    f"#define UI_FONT_N {len(glyphs)}",
    f"#define UI_FONT_H {SIZE}",
    f"#define UI_FONT_W {SIZE}",
    f"#define UI_FONT_BYTES {SIZE * SIZE // 8}",
    "",
    "typedef struct {",
    "    uint32_t cp;",
    "    uint8_t bits[UI_FONT_BYTES];",
    "} ui_glyph_t;",
    "",
    "static const ui_glyph_t s_glyphs[UI_FONT_N] = {",
]
for ch, bits in glyphs:
    hexb = ", ".join(f"0x{b:02X}" for b in bits)
    lines.append(f"    {{ {ord(ch)}, {{ {hexb} }} }}, /* U+{ord(ch):04X} */")
lines += [
    "};",
    "",
    "const uint8_t *ui_font16_bits(uint32_t cp)",
    "{",
    "    for (int i = 0; i < UI_FONT_N; ++i) {",
    "        if (s_glyphs[i].cp == cp) return s_glyphs[i].bits;",
    "    }",
    "    return NULL;",
    "}",
    "",
    "int ui_font16_advance(uint32_t cp)",
    "{",
    "    if (cp < 0x80) return 9;",  # tighter Latin
    f"    return {SIZE};",
    "}",
]
OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
HDR.write_text(
    """#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define UI_FONT16_H 16
#define UI_FONT16_W 16
const uint8_t *ui_font16_bits(uint32_t cp);
int ui_font16_advance(uint32_t cp);
#ifdef __cplusplus
}
#endif
""",
    encoding="utf-8",
)
print(f"glyphs={len(glyphs)} -> {OUT}")
