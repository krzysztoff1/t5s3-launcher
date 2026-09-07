#pragma once
// Page composition. compose() lays out one page from a position and reports
// where the next page starts; paginate() runs it over the whole book so page
// numbers are exact. Both use the same code, and the same font measurements as
// render(), so what the pagination counted is what gets drawn.
#include <stdint.h>

#include "font.h"
#include "textmodel.h"

namespace layout {

struct Style {
    font::Face body, italic, bold;
    float px;          // em size of body text
    int   lineH;       // body line advance
    int   colX, colW;  // text column
    int   top, bottom; // vertical extent of the text box
    bool  justify;
    int   indent;      // first-line indent of body paragraphs
    int   paraGap;     // extra space after body paragraphs (block style)
};

struct Pos {
    uint32_t para = 0;
    uint32_t off = 0;      // byte offset within the paragraph
    uint8_t  italic = 0;   // an _italic_ span is open at this point
};

enum Align : uint8_t { LEFT = 0, CENTER, JUSTIFY };

struct Line {
    uint32_t   s, e;       // absolute byte range (trailing spaces excluded)
    int16_t    x, y;       // pen start, baseline
    float      px;
    float      extra;      // extra px per inter-word space (justification)
    font::Face face;
    uint8_t    italic;     // italic state at s
    uint8_t    align;
    uint8_t    kind;       // text::Kind
};

constexpr int MAX_LINES = 96;

struct Page {
    Line     lines[MAX_LINES];
    int      count = 0;
    Pos      start;
    Pos      next;
    bool     last = false;   // nothing follows this page
};

// False when `start` is at or past the end of the book.
bool compose(const text::Model& m, const Style& st, Pos start, Page& out);
void render(const text::Model& m, const Style& st, const Page& page);

struct Pagination {
    Pos*     pages = nullptr;
    uint32_t count = 0;
    uint32_t cap = 0;
};
bool     paginate(const text::Model& m, const Style& st, Pagination& out);
void     release(Pagination& p);
uint32_t absOffset(const text::Model& m, const Pos& p);
uint32_t pageForOffset(const Pagination& pg, const text::Model& m, uint32_t abs);

}  // namespace layout
