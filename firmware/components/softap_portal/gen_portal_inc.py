#!/usr/bin/env python3
"""Embed portal_page.html as a C string literal (PAGE_HTML)."""
from pathlib import Path

here = Path(__file__).resolve().parent
html = (here / "portal_page.html").read_text(encoding="utf-8")
# Normalize newlines; escape for C string
html = html.replace("\r\n", "\n").replace("\r", "\n")
out_lines = [
    "/* Auto-generated from portal_page.html — do not edit by hand. */",
    "static const char PAGE_HTML[] =",
]
for line in html.split("\n"):
    esc = (
        line.replace("\\", "\\\\")
        .replace('"', '\\"')
    )
    out_lines.append(f'    "{esc}\\n"')
out_lines.append(";")
(here / "portal_page.inc").write_text("\n".join(out_lines) + "\n", encoding="utf-8")
print("wrote", here / "portal_page.inc", "bytes", len(html))
