#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# ///
"""Fetch the public-domain sample books, strip the distributor boilerplate and
write assets/books/<slug>.txt plus src/builtin_books.h (the table the firmware
embeds). Run from anywhere:

    tools/prep_books.py [--raw DIR]      # DIR caches the downloads (default /tmp/paperback-raw)

The reader itself does the reflow (joining hard-wrapped lines, spotting
chapter headings); this only normalises line endings, whitespace and blank
runs so the embedded text is small and predictable.
"""
import argparse
import os
import re
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))
BOOKS_DIR = os.path.join(APP, "assets", "books")
COVERS_DIR = os.path.join(APP, "assets", "covers")
HEADER = os.path.join(APP, "src", "builtin_books.h")
SYMBOL_PREFIX = "_binary_assets_books_"   # what objcopy derives from the embed_files path

# slug, source, id, title, author, year, lang, blurb
BOOKS = [
    ("alice", "gutenberg", "11", "Alice's Adventures in Wonderland", "Lewis Carroll", 1865, "en",
     "A girl follows a hurried rabbit down a hole into a world where logic takes the afternoon off."),
    ("frankenstein", "gutenberg", "84", "Frankenstein", "Mary Shelley", 1818, "en",
     "A young scientist assembles a living being, then spends the rest of his life answering for it."),
    ("christmas_carol", "gutenberg", "46", "A Christmas Carol", "Charles Dickens", 1843, "en",
     "Three spirits give a miser one long night to reconsider everything."),
    ("time_machine", "gutenberg", "35", "The Time Machine", "H. G. Wells", 1895, "en",
     "An inventor travels to the year 802,701 and finds humanity split in two."),
    ("metamorphosis", "gutenberg", "5200", "Metamorphosis", "Franz Kafka", 1915, "en",
     "Gregor Samsa wakes up as an insect. His family adjusts, slowly and badly."),
    ("sherlock_holmes", "gutenberg", "1661", "The Adventures of Sherlock Holmes", "Arthur Conan Doyle", 1892, "en",
     "Twelve cases from Baker Street, told by the friend who was there."),
    ("the_prophet", "gutenberg", "58585", "The Prophet", "Kahlil Gibran", 1923, "en",
     "Before sailing home, Almustafa speaks on love, work, joy, sorrow and the rest of it."),
    ("kamizelka", "wolnelektury", "kamizelka", "Kamizelka", "Bolesław Prus", 1882, "pl",
     "Historia pewnej starej kamizelki i dwojga ludzi, którzy oszukiwali się z miłości."),
    ("latarnik", "wolnelektury", "latarnik", "Latarnik", "Henryk Sienkiewicz", 1881, "pl",
     "Stary tułacz znajduje spokój w latarni morskiej, dopóki nie dostaje książki z kraju."),
]

SOURCES = {
    "gutenberg": ("https://www.gutenberg.org/cache/epub/{id}/pg{id}.txt", "Project Gutenberg #{id}"),
    "wolnelektury": ("https://wolnelektury.pl/media/book/txt/{id}.txt", "Wolne Lektury"),
}


def fetch(url, path):
    if not os.path.exists(path):
        print(f"  fetching {url}")
        with urllib.request.urlopen(url, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    with open(path, "rb") as f:
        return f.read().decode("utf-8-sig")


def strip_gutenberg(text):
    m = re.search(r"^\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG EBOOK.*?\*\*\*\s*$", text, re.M | re.I)
    if m:
        text = text[m.end():]
    m = re.search(r"^\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG EBOOK", text, re.M | re.I)
    if m:
        text = text[:m.start()]
    return text


def strip_wolnelektury(text):
    lines = text.split("\n")
    for i, line in enumerate(lines[:12]):          # author / title / ISBN block
        if line.startswith("ISBN"):
            lines = lines[i + 1:]
            break
    for i, line in enumerate(lines):               # licence and credits after the rule
        if line.startswith("-----"):
            lines = lines[:i]
            break
    return "\n".join(lines)


def normalise(text):
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\t", "    ")
    text = text.replace("­", "")              # soft hyphens
    lines = [l.rstrip() for l in text.split("\n")]
    out, blanks = [], 0
    for l in lines:
        if l == "":
            blanks += 1
            if blanks <= 2:
                out.append(l)
        else:
            blanks = 0
            out.append(l)
    return "\n".join(out).strip("\n") + "\n"


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default="/tmp/paperback-raw")
    args = ap.parse_args()
    os.makedirs(args.raw, exist_ok=True)
    os.makedirs(BOOKS_DIR, exist_ok=True)

    rows, total = [], 0
    for slug, source, ident, title, author, year, lang, blurb in BOOKS:
        url_t, credit_t = SOURCES[source]
        raw = fetch(url_t.format(id=ident), os.path.join(args.raw, f"{source}-{ident}.txt"))
        text = strip_gutenberg(raw) if source == "gutenberg" else strip_wolnelektury(raw)
        text = normalise(text)
        path = os.path.join(BOOKS_DIR, f"{slug}.txt")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        size = len(text.encode("utf-8"))
        total += size
        print(f"  {slug:16s} {size:8d} B  {title} - {author}")
        rows.append((slug, title, author, year, lang, credit_t.format(id=ident), blurb))

    with open(HEADER, "w", encoding="utf-8") as h:
        h.write("// Generated by tools/prep_books.py - do not edit. Texts are embedded via\n"
                "// board_build.embed_files in platformio.ini; objcopy names the symbols after\n"
                "// the file path.\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
                "struct BuiltinBook {\n    const char*    title;\n    const char*    author;\n"
                "    const char*    source;\n    const char*    blurb;\n    const char*    lang;\n"
                "    uint16_t       year;\n    const uint8_t* start;\n    const uint8_t* end;\n"
                "    const uint8_t* coverStart;   // PNG, or nullptr\n    const uint8_t* coverEnd;\n};\n\n")
        for slug, *_ in rows:
            sym = f"{SYMBOL_PREFIX}{slug}_txt"
            h.write(f'extern "C" const uint8_t {sym}_start[] asm("{sym}_start");\n')
            h.write(f'extern "C" const uint8_t {sym}_end[]   asm("{sym}_end");\n')
            if os.path.exists(os.path.join(COVERS_DIR, f"{slug}.png")):
                csym = f"_binary_assets_covers_{slug}_png"
                h.write(f'extern "C" const uint8_t {csym}_start[] asm("{csym}_start");\n')
                h.write(f'extern "C" const uint8_t {csym}_end[]   asm("{csym}_end");\n')
        h.write("\nstatic const BuiltinBook kBuiltinBooks[] = {\n")
        for slug, title, author, year, lang, credit, blurb in rows:
            sym = f"{SYMBOL_PREFIX}{slug}_txt"
            if os.path.exists(os.path.join(COVERS_DIR, f"{slug}.png")):
                csym = f"_binary_assets_covers_{slug}_png"
                cover = f"{csym}_start, {csym}_end"
            else:
                cover = "nullptr, nullptr"
            h.write(f"    {{{c_str(title)}, {c_str(author)}, {c_str(credit)},\n"
                    f"     {c_str(blurb)},\n"
                    f"     {c_str(lang)}, {year}, {sym}_start, {sym}_end, {cover}}},\n")
        h.write("};\nstatic const size_t kBuiltinBookCount = sizeof(kBuiltinBooks) / sizeof(kBuiltinBooks[0]);\n")
    print(f"  total {total} B in {len(rows)} books -> {os.path.relpath(HEADER, APP)}")


if __name__ == "__main__":
    main()
