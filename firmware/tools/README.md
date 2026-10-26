# Firmware tools

## Regenerate the screen fonts

The round screen uses two small bitmap fonts compiled into the firmware
(`components/ui/fonts/`). They are generated from open-licensed fonts:

| Font | Used for | License | Get it |
|---|---|---|---|
| Noto Sans CJK SC Regular | 16 px UI text (Chinese, Japanese, Korean, Latin, Cyrillic) | SIL OFL 1.1 | [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk) → `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` |
| Fredoka | 32 px clock digits | SIL OFL 1.1 | [google/fonts](https://github.com/google/fonts/tree/main/ofl/fredoka) → `Fredoka[wdth,wght].ttf` |

1. Put the files in `firmware/tools/fonts/` as `NotoSansCJKsc-Regular.otf` and `Fredoka.ttf`
   (or set `UI_FONT` / `UI_DIGIT_FONT` to their paths).
2. `pip install pillow`
3. `python tools/gen_ui_font16.py` and `python tools/gen_ui_font32.py`

Only the characters listed in `gen_ui_font16.py` are included. If you add new
on-screen text in another script, add its characters there and regenerate.
