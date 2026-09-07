#include "layout.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "utf8.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
static void* bigAlloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
static void  bigFree(void* p) { heap_caps_free(p); }
#else
#include <stdlib.h>
static void* bigAlloc(void* p, size_t n) { return realloc(p, n); }
static void  bigFree(void* p) { free(p); }
#endif

namespace font {
float glyphAdvance(Face f, float px, uint32_t cp, int& prevGi);
float drawGlyph(float penX, int y, Face f, float px, uint32_t cp, int& prevGi, uint8_t ink);
}

namespace display {
void fillCircle(int cx, int cy, int r, uint8_t c);
}

namespace layout {

static inline bool isSpace(uint32_t cp) { return cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r'; }
static inline bool isWordChar(uint32_t cp) { return cp < 0x80 ? isalnum((int)cp) != 0 : cp < 0x2000; }
// A Gutenberg _italic_ marker sits at a word boundary; snake_case does not.
static inline bool underscoreToggles(uint32_t prevCp, uint32_t nextCp) {
    return !isWordChar(prevCp) || !isWordChar(nextCp);
}
static inline uint32_t peekCp(const char* p, const char* pe) {
    if (p >= pe) return ' ';
    return utf8Next(p, pe);
}

struct KindStyle {
    font::Face face;
    float px;
    int   lh;
    Align align;
    bool  allowItalic;
};

static KindStyle kindStyle(const Style& st, uint8_t kind) {
    KindStyle k{st.body, st.px, st.lineH, st.justify ? JUSTIFY : LEFT, true};
    switch (kind) {
        case text::H1:
            k.face = st.bold; k.px = st.px * 1.45f; k.lh = (int)lroundf(k.px * 1.3f); k.align = CENTER; k.allowItalic = false; break;
        case text::H2:
            k.face = st.bold; k.px = st.px * 1.12f; k.lh = (int)lroundf(k.px * 1.4f); k.align = CENTER; k.allowItalic = false; break;
        case text::SUBTITLE:
            k.face = st.italic; k.px = st.px * 1.12f; k.lh = (int)lroundf(k.px * 1.4f); k.align = CENTER; k.allowItalic = false; break;
        case text::VERSE:
            k.align = LEFT; break;
        case text::SEPARATOR:
            k.align = CENTER; k.allowItalic = false; break;
        default: break;
    }
    return k;
}

// Walks the glyphs of the word at p (p is not a space). Measures it, or with
// `avail` >= 0 stops before the first glyph that would exceed avail (keeping at
// least one). Updates italic to the state after the consumed part; sets end.
static float walkWord(const char* p, const char* pe, const Style& st, const KindStyle& ks,
                      bool& italic, const char*& end, float avail) {
    float w = 0.f;
    int prev = -1;
    int glyphs = 0;
    uint32_t prevCp = ' ';
    font::Face face = (ks.allowItalic && italic) ? st.italic : ks.face;
    const char* consumed = p;
    while (p < pe) {
        const char* q = p;
        const uint32_t cp = utf8Next(q, pe);
        if (isSpace(cp)) break;
        if (cp == '_' && ks.allowItalic && underscoreToggles(prevCp, peekCp(q, pe))) {
            italic = !italic;
            face = italic ? st.italic : ks.face;
            prev = -1;
            p = consumed = q;
            prevCp = cp;
            continue;
        }
        if (utf8Invisible(cp)) { p = consumed = q; continue; }
        const float a = font::glyphAdvance(face, ks.px, cp, prev);
        if (avail >= 0.f && glyphs > 0 && w + a > avail) break;
        w += a;
        glyphs++;
        prevCp = cp;
        p = consumed = q;
    }
    end = consumed;
    return w;
}

bool compose(const text::Model& m, const Style& st, Pos start, Page& out) {
    out.count = 0;
    out.start = start;
    out.last = false;
    if (start.para >= m.count) { out.next = start; out.last = true; return false; }

    int y = st.top;                       // top of the next line box
    uint32_t p = start.para;
    uint32_t off = start.off;
    bool italic = start.italic != 0;
    bool placedAny = false;

    while (p < m.count) {
        const text::Para& P = m.paras[p];
        const KindStyle ks = kindStyle(st, P.kind);
        const char* ps = m.text + P.start;
        const char* pe = ps + P.len;
        if (off > P.len) off = P.len;
        const bool fresh = (off == 0);

        if (fresh) {
            italic = false;
            int gap = 0;
            if (P.kind == text::H1) {
                if (out.count > 0) { out.next = Pos{p, 0, 0}; return true; }   // chapters start a page
                gap = st.lineH * 2;
            } else if (out.count > 0) {
                switch (P.kind) {
                    case text::H2:        gap = st.lineH; break;
                    case text::SUBTITLE:  gap = 0; break;
                    case text::SEPARATOR: gap = st.lineH / 2; break;
                    case text::VERSE:     gap = (P.flags & text::F_BLANK_BEFORE) ? st.lineH / 2 : 0; break;
                    default:              gap = st.paraGap + ((P.flags & text::F_GAP) ? st.lineH / 2 : 0); break;
                }
            }
            if (out.count > 0 && y + gap + ks.lh > st.bottom) { out.next = Pos{p, 0, 0}; return true; }
            y += gap;
        }

        if (P.kind == text::SEPARATOR) {
            if (out.count >= MAX_LINES) { out.next = Pos{p, 0, 0}; return true; }
            Line& L = out.lines[out.count++];
            L.s = L.e = P.start; L.x = (int16_t)(st.colX + st.colW / 2); L.y = (int16_t)(y + ks.lh / 2);
            L.px = ks.px; L.extra = 0; L.face = ks.face; L.italic = 0; L.align = CENTER; L.kind = P.kind;
            y += ks.lh;
            placedAny = true;
            p++; off = 0;
            continue;
        }

        int firstIndent = 0, restIndent = 0;
        if (P.kind == text::BODY) {
            firstIndent = (st.paraGap > 0 || (P.flags & text::F_AFTER_HEADING)) ? 0 : st.indent;
        } else if (P.kind == text::VERSE) {
            const int lvl = P.indent > 16 ? 16 : P.indent;
            firstIndent = (int)lroundf(lvl * st.px * 0.28f);
            restIndent = firstIndent + (int)lroundf(st.px * 1.5f);
        }

        const char* cur = ps + off;
        bool lineHasWords = false;
        const char* lineStart = cur;
        bool lineItalic = italic;
        const char* lastWordEnd = cur;
        float lineW = 0.f;
        int spaces = 0;
        const float spaceW = font::advance(ks.face, ks.px, ' ');
        int indentNow = fresh ? firstIndent : restIndent;
        bool pageFull = false;

        // Places the pending line. False when the page is full (line not placed).
        auto emit = [&](bool lastLine) -> bool {
            if (out.count > 0 && (y + ks.lh > st.bottom || out.count >= MAX_LINES)) return false;
            Line& L = out.lines[out.count++];
            L.s = (uint32_t)(lineStart - m.text);
            L.e = (uint32_t)(lastWordEnd - m.text);
            L.px = ks.px; L.face = ks.face; L.italic = lineItalic ? 1 : 0; L.kind = P.kind;
            L.y = (int16_t)(y + (int)lroundf(font::ascent(ks.face, ks.px) + (ks.lh - font::lineHeight(ks.face, ks.px)) / 2));
            const int avail = st.colW - indentNow;
            L.extra = 0.f;
            if (ks.align == CENTER) {
                L.align = CENTER;
                L.x = (int16_t)(st.colX + (int)lroundf((st.colW - lineW) / 2));
            } else if (ks.align == JUSTIFY && !lastLine && spaces > 0) {
                const float extra = (avail - lineW) / spaces;
                if (extra <= spaceW * 2.5f) { L.align = JUSTIFY; L.extra = extra; }
                else L.align = LEFT;
                L.x = (int16_t)(st.colX + indentNow);
            } else {
                L.align = LEFT;
                L.x = (int16_t)(st.colX + indentNow);
            }
            y += ks.lh;
            placedAny = true;
            return true;
        };
        auto newLine = [&](const char* at) {
            lineHasWords = false; lineW = 0.f; spaces = 0;
            indentNow = restIndent;
            lineStart = at; lineItalic = italic;
        };
        auto onlySpacesFrom = [&](const char* t) {
            while (t < pe) { const char* q = t; if (!isSpace(utf8Next(q, pe))) return false; t = q; }
            return true;
        };

        while (true) {
            const char* w = cur;
            while (w < pe) {
                const char* q = w;
                if (!isSpace(utf8Next(q, pe))) break;
                w = q;
            }
            if (w >= pe) {
                if (lineHasWords && !emit(true)) {
                    pageFull = true;
                    out.next = Pos{p, (uint32_t)(lineStart - ps), (uint8_t)(lineItalic ? 1 : 0)};
                }
                break;
            }
            if (!lineHasWords) { lineStart = w; lineItalic = italic; }

            const char* wordEnd;
            bool italicAfter = italic;
            const float wordW = walkWord(w, pe, st, ks, italicAfter, wordEnd, -1.f);
            const float sep = lineHasWords ? spaceW : 0.f;

            if (lineHasWords && lineW + sep + wordW > st.colW - indentNow) {
                if (!emit(false)) {
                    pageFull = true;
                    out.next = Pos{p, (uint32_t)(lineStart - ps), (uint8_t)(lineItalic ? 1 : 0)};
                    break;
                }
                newLine(w);
            }
            const int avail = st.colW - indentNow;
            if (wordW > avail) {
                // Wider than the column: the longest prefix that fits becomes a line of its own.
                bool italicCut = italic;
                const char* cut;
                const float pw = walkWord(w, pe, st, ks, italicCut, cut, (float)avail);
                if (cut <= w) { cut = w; utf8Next(cut, pe); }
                lineStart = w; lineItalic = italic; lastWordEnd = cut; lineW = pw; spaces = 0; lineHasWords = true;
                const bool lastLine = onlySpacesFrom(cut);
                if (!emit(lastLine)) {
                    pageFull = true;
                    out.next = Pos{p, (uint32_t)(lineStart - ps), (uint8_t)(lineItalic ? 1 : 0)};
                    break;
                }
                italic = italicCut;
                newLine(cut);
                cur = cut;
                continue;
            }
            if (lineHasWords) { lineW += spaceW; spaces++; }
            lineW += wordW;
            lineHasWords = true;
            lastWordEnd = wordEnd;
            italic = italicAfter;
            cur = wordEnd;
        }
        if (pageFull) return true;

        if (P.kind == text::H1) {
            const bool subNext = (p + 1 < m.count && m.paras[p + 1].kind == text::SUBTITLE);
            y += subNext ? (int)(ks.lh * 0.15f) : (int)(st.lineH * 0.9f);
        } else if (P.kind == text::SUBTITLE) {
            y += (int)(st.lineH * 0.9f);
        } else if (P.kind == text::H2) {
            y += st.lineH / 3;
        }
        p++;
        off = 0;
        italic = false;
    }
    out.next = Pos{m.count, 0, 0};
    out.last = true;
    return placedAny;
}

void render(const text::Model& m, const Style& st, const Page& page) {
    for (int i = 0; i < page.count; ++i) {
        const Line& L = page.lines[i];
        if (L.kind == text::SEPARATOR) {
            const int r = L.px >= 30 ? 3 : 2;
            const int gap = (int)(L.px * 0.7f);
            display::fillCircle(L.x - gap, L.y, r, 3);
            display::fillCircle(L.x, L.y, r, 3);
            display::fillCircle(L.x + gap, L.y, r, 3);
            continue;
        }
        const KindStyle ks = kindStyle(st, L.kind);
        const char* s = m.text + L.s;
        const char* e = m.text + L.e;
        bool italic = L.italic != 0;
        font::Face face = (ks.allowItalic && italic) ? st.italic : L.face;
        float pen = (float)L.x;
        int prev = -1;
        uint32_t prevCp = ' ';
        const float spaceW = font::advance(L.face, L.px, ' ');
        while (s < e) {
            const char* q = s;
            const uint32_t cp = utf8Next(q, e);
            if (isSpace(cp)) {
                s = q;
                while (s < e) { const char* r = s; if (!isSpace(utf8Next(r, e))) break; s = r; }
                pen += spaceW + L.extra;
                prev = -1;
                prevCp = ' ';
                continue;
            }
            if (cp == '_' && ks.allowItalic && underscoreToggles(prevCp, peekCp(q, e))) {
                italic = !italic;
                face = italic ? st.italic : L.face;
                prev = -1;
                prevCp = cp;
                s = q;
                continue;
            }
            s = q;
            if (utf8Invisible(cp)) continue;
            pen += font::drawGlyph(pen, L.y, face, L.px, cp, prev, 3);
            prevCp = cp;
        }
    }
}

bool paginate(const text::Model& m, const Style& st, Pagination& out) {
    release(out);
    Page* page = (Page*)bigAlloc(nullptr, sizeof(Page));
    if (!page) return false;
    Pos pos{0, 0, 0};
    uint32_t guard = 0;
    while (pos.para < m.count) {
        if (!compose(m, st, pos, *page)) break;
        if (out.count == out.cap) {
            const uint32_t ncap = out.cap ? out.cap * 2 : 256;
            Pos* np = (Pos*)bigAlloc(out.pages, sizeof(Pos) * ncap);
            if (!np) { bigFree(page); return false; }
            out.pages = np; out.cap = ncap;
        }
        out.pages[out.count++] = pos;
        if (page->last) break;
        if (page->next.para == pos.para && page->next.off == pos.off) page->next.off++;   // never stall
        pos = page->next;
        if (++guard > 50000) break;
    }
    bigFree(page);
    return out.count > 0;
}

void release(Pagination& p) {
    if (p.pages) bigFree(p.pages);
    p.pages = nullptr; p.count = p.cap = 0;
}

uint32_t absOffset(const text::Model& m, const Pos& p) {
    if (p.para >= m.count) return m.len;
    return m.paras[p.para].start + p.off;
}

uint32_t pageForOffset(const Pagination& pg, const text::Model& m, uint32_t abs) {
    if (pg.count == 0) return 0;
    uint32_t lo = 0, hi = pg.count - 1;
    while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        if (absOffset(m, pg.pages[mid]) <= abs) lo = mid; else hi = mid - 1;
    }
    return lo;
}

}  // namespace layout
