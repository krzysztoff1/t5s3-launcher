#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=10"]
# ///
"""Fetch the sample books' cover art and turn it into small greyscale PNGs the
firmware embeds (assets/covers/<slug>.png, 180x252, 8-bit grey). Run from
anywhere; re-run prep_books.py afterwards so builtin_books.h points at them.

    tools/fetch_covers.py [--raw DIR]

Sources: Project Gutenberg's per-book cover (pg<id>.cover.medium.jpg) and the
Wolne Lektury API ("cover" field). Images are centre-cropped to 5:7, contrast
stretched a little (e-paper has four greys) and lightly sharpened.
"""
import argparse
import io
import json
import os
import urllib.request

from PIL import Image, ImageFilter, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(APP, "assets", "covers")
W, H = 180, 252

# slug -> (source, id)  — keep in step with BOOKS in prep_books.py
COVERS = {
    "alice": ("gutenberg", "11"),
    "frankenstein": ("gutenberg", "84"),
    "christmas_carol": ("gutenberg", "46"),
    "time_machine": ("gutenberg", "35"),
    "metamorphosis": ("gutenberg", "5200"),
    "sherlock_holmes": ("gutenberg", "1661"),
    "the_prophet": ("gutenberg", "58585"),
    "kamizelka": ("wolnelektury", "kamizelka"),
    "latarnik": ("wolnelektury", "latarnik"),
}


def fetch(url, path):
    if not os.path.exists(path):
        print(f"  fetching {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "paperback-tools/0.1"})
        with urllib.request.urlopen(req, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    with open(path, "rb") as f:
        return f.read()


def cover_url(source, ident, raw_dir):
    if source == "gutenberg":
        return f"https://www.gutenberg.org/cache/epub/{ident}/pg{ident}.cover.medium.jpg"
    meta = json.loads(fetch(f"https://wolnelektury.pl/api/books/{ident}/", os.path.join(raw_dir, f"wl-{ident}.json")))
    return meta["cover"]


def convert(data):
    img = Image.open(io.BytesIO(data)).convert("L")
    img = ImageOps.fit(img, (W, H), method=Image.LANCZOS, centering=(0.5, 0.4))
    img = ImageOps.autocontrast(img, cutoff=1)
    img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default="/tmp/paperback-raw")
    args = ap.parse_args()
    os.makedirs(args.raw, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    for slug, (source, ident) in COVERS.items():
        url = cover_url(source, ident, args.raw)
        data = fetch(url, os.path.join(args.raw, f"cover-{slug}.jpg"))
        img = convert(data)
        path = os.path.join(OUT, f"{slug}.png")
        img.save(path, format="PNG", optimize=True)
        print(f"  {slug:16s} {os.path.getsize(path):6d} B  from {url}")


if __name__ == "__main__":
    main()
