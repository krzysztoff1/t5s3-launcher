#pragma once
// Turns a plain-text book into paragraphs. Plain text as distributed (Project
// Gutenberg, Wolne Lektury, a file someone typed) is hard-wrapped at ~70
// columns with blank lines between paragraphs, so lines inside a block are
// joined and the block becomes one reflowable paragraph. Indented lines are
// verse and keep their breaks; short isolated lines that look like chapter
// headings become headings, which also gives the book a table of contents.
#include <stddef.h>
#include <stdint.h>

namespace text {

enum Kind : uint8_t { BODY = 0, VERSE, H1, H2, SUBTITLE, SEPARATOR };
enum Flags : uint8_t {
    F_GAP = 1,             // two or more blank lines before it (section break)
    F_BLANK_BEFORE = 2,    // at least one blank line before it
    F_AFTER_HEADING = 4,   // previous paragraph is a heading or subtitle
    F_ROMAN = 8,           // heading text is a bare roman/arabic numeral
};

struct Para {
    uint32_t start;        // byte offset into the text
    uint32_t len;          // bytes; body paragraphs contain the source '\n's
    uint8_t  kind;
    uint8_t  indent;       // leading spaces (verse)
    uint8_t  flags;
    uint8_t  level;        // unused for now
};

struct Model {
    const char* text = nullptr;
    uint32_t    len = 0;
    Para*       paras = nullptr;
    uint32_t    count = 0;
    uint32_t    cap = 0;
    uint32_t*   chapters = nullptr;    // indices into paras (H1 headings)
    uint32_t    chapterCount = 0;
    uint32_t    words = 0;             // rough word count
};

bool build(Model& m, const char* text, uint32_t len);
void release(Model& m);

// Index of the paragraph containing byte offset `abs` (the last one starting at or before it).
uint32_t paraForOffset(const Model& m, uint32_t abs);
// Human title for chapter i: heading text, plus the subtitle if there is one.
void chapterTitle(const Model& m, uint32_t chapter, char* buf, size_t cap);
// Chapter index for a byte offset (the last chapter starting at or before it), or -1.
int chapterForOffset(const Model& m, uint32_t abs);

}  // namespace text
