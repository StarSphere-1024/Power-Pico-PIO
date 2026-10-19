#!/usr/bin/env python3
"""Generate embedded LVGL fonts from the licensed, vendored source fonts."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "src/app/User/GUI/font"
CONVERTER = FONT_DIR / "node_modules/.bin/lv_font_conv"
BARLOW = FONT_DIR / "BarlowSemiCondensed-Medium.ttf"
GLOW = FONT_DIR / "GlowSansSC-Normal-Regular.otf"
BARLOW_SHA256 = "4998b693a190b76f2b11315844345a8546c1e2e6a320905507a9eed04edf8b48"
GLOW_SHA256 = "ca7d34947401b2fa6dd4fd46676d970a8cc47a40ee60c022eb7dd5068bee67ff"
BARLOW_SOURCE = "https://github.com/jpt/barlow/tree/b4726ddf91525818e85e5fce111c285b9273d764"
GLOW_SOURCE = "https://github.com/welai/glow-sans/releases/tag/v0.93"

BARLOW_GLYPHS = {size: None for size in (14, 16, 18, 20, 22, 28)}
BARLOW_GLYPHS.update({32: "0123456789.-:VµAmW", 48: "0123456789.-:"})
ZH_MESSAGES = {
    18: (
        "Screen Brightness :",
        "Enable key sound",
        "Enable Chinese",
        "Chose Rotation",
        "Current Range",
        "AUTO",
        "LOW",
        "MID",
        "HIGH",
    ),
    20: (
        "press to Step Adjust",
        "press here to Fixed Set",
        "press here to close PPS",
        "PD Sink",
        "Please reboot to apply\nall changes!",
    ),
}
GLOW_EXTRA_GLYPHS = {18: "▶", 20: ""}


def check_source(path: Path, expected: str) -> None:
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"{path.name}: SHA-256 mismatch: {actual}")


def tnum_font(path: Path, output: Path) -> None:
    font = TTFont(path)
    gsub = font["GSUB"].table
    lookup_indices = {
        index
        for record in gsub.FeatureList.FeatureRecord
        if record.FeatureTag == "tnum"
        for index in record.Feature.LookupListIndex
    }
    substitutions: dict[str, str] = {}
    for index in lookup_indices:
        lookup = gsub.LookupList.Lookup[index]
        if lookup.LookupType != 1:
            raise SystemExit(f"unsupported Barlow tnum lookup type {lookup.LookupType}")
        for subtable in lookup.SubTable:
            substitutions.update(subtable.mapping)

    for codepoint in range(ord("0"), ord("9") + 1):
        glyph = font.getBestCmap().get(codepoint)
        replacement = substitutions.get(glyph)
        if replacement is None:
            raise SystemExit(f"Barlow tnum feature lacks {chr(codepoint)!r}")
        for cmap in font["cmap"].tables:
            if cmap.isUnicode() and codepoint in cmap.cmap:
                cmap.cmap[codepoint] = replacement

    advances = {
        font["hmtx"][font.getBestCmap()[codepoint]][0]
        for codepoint in range(ord("0"), ord("9") + 1)
    }
    if len(advances) != 1:
        raise SystemExit("Barlow tabular digit advances are not equal")

    # LVGL has no OpenType shaping. The cmap now points directly at tnum outlines;
    # removing shaping tables also prevents the source's kern feature being applied.
    del font["GSUB"]
    if "GPOS" in font:
        del font["GPOS"]
    if "kern" in font:
        del font["kern"]
    font.save(output)


def glow_symbols(font_size: int) -> str:
    source = (ROOT / "src/app/User/GUI/common/lv_i18n.c").read_text(encoding="utf-8")
    marker = "static lv_i18n_phrase_t zh_cn_singulars[] = {"
    if marker not in source:
        raise SystemExit("Chinese translation array not found in lv_i18n.c")
    table = source.split(marker, 1)[1].split("};", 1)[0]
    pairs = re.findall(
        r'^\s*\{\s*("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")\s*\}',
        table,
        re.MULTILINE,
    )
    translations = {json.loads(key): json.loads(value) for key, value in pairs}
    missing = [key for key in ZH_MESSAGES[font_size] if key not in translations]
    if missing:
        raise SystemExit(f"Chinese translations missing for {font_size}px font: {missing}")
    text = "".join(translations[key] for key in ZH_MESSAGES[font_size])
    glyphs = {char for char in text if "\u4e00" <= char <= "\u9fff"}
    glyphs.update(GLOW_EXTRA_GLYPHS[font_size])
    return "".join(sorted(glyphs))


def convert(
    source: Path,
    output: Path,
    symbol: str,
    size: int,
    glyphs: str | None = None,
    fallback: str | None = None,
) -> None:
    command = [
        str(CONVERTER),
        "--bpp",
        "4",
        "--size",
        str(size),
        "--font",
        source.relative_to(ROOT).as_posix(),
        "--output",
        output.relative_to(ROOT).as_posix(),
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
        "--lv-font-name",
        symbol,
        "--no-compress",
        "--no-prefilter",
        "--no-kerning",
    ]
    if glyphs is None:
        command.extend(("--range", "0x20-0x7e", "--symbols", "µ"))
    else:
        command.extend(("--symbols", glyphs))
    if fallback:
        command.extend(("--lv-fallback", fallback))
    subprocess.run(command, cwd=ROOT, check=True)


def generated_header(symbol: str) -> str:
    if symbol.startswith("ui_font_barlow_"):
        return (
            "/* Generated by assets/fonts/generate_fonts.py; do not edit.\n"
            " * Source: Barlow Semi Condensed Medium, version 1.408.\n"
            f" * Upstream: {BARLOW_SOURCE}\n"
            f" * SHA-256: {BARLOW_SHA256}\n"
            " * Copyright 2017 The Barlow Project Authors (https://github.com/jpt/barlow).\n"
            " * SIL OFL 1.1; see assets/fonts/OFL-Barlow.txt. Digits use statically\n"
            " * mapped tnum alternates; GSUB/GPOS and converter kerning are disabled.\n"
            " */\n"
        )
    return (
        "/* Generated by assets/fonts/generate_fonts.py; do not edit.\n"
        " * Source: Glow Sans SC Normal Regular, version 0.93.\n"
        f" * Upstream: {GLOW_SOURCE}\n"
        f" * SHA-256: {GLOW_SHA256}\n"
        " * Copyright (c) 2020, Celestial Phineas. SIL OFL 1.1; see\n"
        " * assets/fonts/OFL-GlowSansSC.txt. Chinese UI glyphs plus U+25B6 play\n"
        " * triangle; Latin and punctuation fall back to matching Barlow 18/20.\n"
        " */\n"
    )


def finalize_c(path: Path, symbol: str) -> None:
    text = path.read_text(encoding="utf-8")
    # lv_font_conv records input paths in its generated banner; keep output stable.
    text = re.sub(r"^\s*\* Opts:.*\n", "", text, flags=re.MULTILINE)
    path.write_text(generated_header(symbol) + text, encoding="utf-8", newline="\n")


def write_header() -> None:
    symbols = [*(f"ui_font_barlow_{size}" for size in (14, 16, 18, 20, 22, 28, 32, 48)),
               "ui_font_glow_18", "ui_font_glow_20"]
    declarations = "\n".join(f"LV_FONT_DECLARE({symbol});" for symbol in symbols)
    header = (
        "/* Generated by assets/fonts/generate_fonts.py; do not edit. */\n"
        "#ifndef UI_FONTS_H\n#define UI_FONTS_H\n\n"
        "#include \"lvgl.h\"\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        f"{declarations}\n\n"
        "#ifdef __cplusplus\n}\n#endif\n\n"
        "#endif /* UI_FONTS_H */\n"
    )
    (OUTPUT_DIR / "ui_fonts.h").write_text(header, encoding="utf-8", newline="\n")


def main() -> None:
    if not CONVERTER.is_file():
        raise SystemExit("Run `npm ci --prefix assets/fonts` before generation")
    check_source(BARLOW, BARLOW_SHA256)
    check_source(GLOW, GLOW_SHA256)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    tnum_source = FONT_DIR / ".BarlowSemiCondensed-Medium-tnum.ttf"
    try:
        tnum_font(BARLOW, tnum_source)
        for size, glyphs in BARLOW_GLYPHS.items():
            symbol = f"ui_font_barlow_{size}"
            output = OUTPUT_DIR / f"{symbol}.c"
            convert(tnum_source, output, symbol, size, glyphs)
            finalize_c(output, symbol)

        glow_font = TTFont(GLOW)
        cmap = glow_font.getBestCmap()
        for size in (18, 20):
            glyphs = glow_symbols(size)
            missing = sorted(char for char in glyphs if ord(char) not in cmap)
            if missing:
                raise SystemExit(f"Glow Sans lacks required {size}px glyphs: {missing}")
            symbol = f"ui_font_glow_{size}"
            output = OUTPUT_DIR / f"{symbol}.c"
            convert(
                GLOW,
                output,
                symbol,
                size,
                glyphs,
                fallback=f"ui_font_barlow_{size}",
            )
            finalize_c(output, symbol)
        write_header()
    finally:
        tnum_source.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
