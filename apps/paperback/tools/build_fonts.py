#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["fonttools>=4.50", "skia-pathops>=0.8"]
# ///
"""Turn the variable Literata and Inter fonts into the five small static faces
Paperback embeds. Run from the app directory:

    tools/build_fonts.py <dir-with-variable-ttfs>

Sources (SIL Open Font License, copies in assets/fonts/OFL-*.txt):
    https://github.com/google/fonts/tree/main/ofl/literata
    https://github.com/google/fonts/tree/main/ofl/inter

Each face is pinned to one weight and one optical size, then subset to Latin,
Latin Extended-A/B, general punctuation and a few symbols, with only the kern
feature kept. GPOS extension lookups are demoted to plain lookups because
stb_truetype only walks lookup type 2.
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "fonts")

FACES = [
    ("Literata[opsz,wght].ttf",        {"opsz": 14, "wght": 400}, "LiterataR.ttf"),
    ("Literata[opsz,wght].ttf",        {"opsz": 14, "wght": 700}, "LiterataB.ttf"),
    ("Literata-Italic[opsz,wght].ttf", {"opsz": 14, "wght": 400}, "LiterataI.ttf"),
    ("Inter[opsz,wght].ttf",           {"opsz": 14, "wght": 400}, "InterR.ttf"),
    ("Inter[opsz,wght].ttf",           {"opsz": 14, "wght": 600}, "InterSB.ttf"),
]

RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A (Polish, Czech, ...)
    (0x0180, 0x024F),  # Latin Extended-B (Romanian, ...)
    (0x2010, 0x2027),  # dashes, quotes, bullets, ellipsis
    (0x2030, 0x203A),  # per mille, primes, guillemets
    (0x2044, 0x2044),
    (0x20AC, 0x20AC),  # euro
    (0x2113, 0x2113), (0x2122, 0x2122), (0x2126, 0x2126),
    (0x2190, 0x2199),  # arrows
    (0x2212, 0x2212), (0x2215, 0x2215), (0x2260, 0x2265),
    (0x25A0, 0x25FF),  # geometric shapes
    (0x2605, 0x2606), (0x2610, 0x2611), (0x2713, 0x2714),
]


def unicodes():
    for lo, hi in RANGES:
        yield from range(lo, hi + 1)


def demote_extension_lookups(font):
    if "GPOS" not in font:
        return 0
    lookups = font["GPOS"].table.LookupList
    if lookups is None:
        return 0
    n = 0
    for lk in lookups.Lookup:
        if lk.LookupType == 9:
            lk.LookupType = lk.SubTable[0].ExtensionLookupType
            lk.SubTable = [st.ExtSubTable for st in lk.SubTable]
            n += 1
    return n


def build(src_dir):
    os.makedirs(OUT_DIR, exist_ok=True)
    for src, axes, out in FACES:
        font = TTFont(os.path.join(src_dir, src))
        try:
            font = instancer.instantiateVariableFont(
                font, axes, inplace=True, overlap=instancer.OverlapMode.REMOVE)
        except Exception as e:  # skia-pathops missing or unhappy
            print(f"  overlap removal unavailable ({e}); keeping overlaps")
            font = TTFont(os.path.join(src_dir, src))
            font = instancer.instantiateVariableFont(font, axes, inplace=True)

        opts = subset.Options()
        opts.layout_features = ["kern"]
        opts.hinting = False
        opts.desubroutinize = True
        opts.notdef_outline = True
        opts.name_IDs = [0, 1, 2, 4, 6]
        opts.drop_tables += ["DSIG", "STAT", "MVAR", "meta", "vhea", "vmtx", "GSUB"]
        s = subset.Subsetter(options=opts)
        s.populate(unicodes=list(unicodes()))
        s.subset(font)
        demoted = demote_extension_lookups(font)

        path = os.path.join(OUT_DIR, out)
        font.save(path)
        font = TTFont(path)
        types = sorted({lk.LookupType for lk in font["GPOS"].table.LookupList.Lookup}) if "GPOS" in font else []
        cmaps = sorted({t.format for t in font["cmap"].tables})
        print(f"  {out:14s} {os.path.getsize(path):7d} B  glyphs={len(font.getGlyphOrder()):4d}"
              f"  gpos={types} cmap={cmaps} demoted={demoted}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build(sys.argv[1])
