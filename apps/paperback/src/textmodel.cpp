#include "textmodel.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
static void* bigAlloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
static void  bigFree(void* p) { heap_caps_free(p); }
#else
#include <stdlib.h>
static void* bigAlloc(void* p, size_t n) { return realloc(p, n); }
static void  bigFree(void* p) { free(p); }
#endif

namespace text {

struct LineInfo {
    const char* s;      // line start
    const char* e;      // line end (excludes '\n')
    const char* ts;     // first non-space
    int indent;
    bool blank;
    bool valid;
};

static LineInfo lineAt(const char* p, const char* end) {
    LineInfo L{};
    L.valid = p < end;
    if (!L.valid) { L.blank = true; L.s = L.e = L.ts = end; return L; }
    L.s = p;
    const char* e = p;
    while (e < end && *e != '\n') ++e;
    L.e = e;
    if (L.e > L.s && L.e[-1] == '\r') --L.e;
    const char* ts = L.s;
    int indent = 0;
    while (ts < L.e && (*ts == ' ' || *ts == '\t')) { indent += (*ts == '\t') ? 4 : 1; ++ts; }
    L.ts = ts;
    L.indent = indent;
    L.blank = ts >= L.e;
    return L;
}

static const char* nextLineStart(const LineInfo& L, const char* end) {
    const char* p = L.e;
    while (p < end && *p != '\n') ++p;    // skip a stray '\r' we trimmed
    return p < end ? p + 1 : end;
}

static bool isRoman(const char* s, const char* e) {
    if (s >= e || e - s > 8) return false;
    for (const char* p = s; p < e; ++p) if (!strchr("IVXLC", *p)) return false;
    return true;
}
static bool isDigits(const char* s, const char* e) {
    if (s >= e || e - s > 3) return false;
    for (const char* p = s; p < e; ++p) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

static bool wordIs(const char* s, const char* e, const char* w) {
    const size_t n = strlen(w);
    return (size_t)(e - s) == n && strncasecmp(s, w, n) == 0;
}

static const char* const kNumberedHeads[] = {
    "CHAPTER", "BOOK", "PART", "LETTER", "STAVE", "ADVENTURE", "VOLUME", "CANTO", "ACT", "SCENE",
    "SECTION", "TALE", "STORY", "LESSON", "ROZDZIAŁ", "Rozdział", "KSIĘGA", "Księga", "CZĘŚĆ", "Część",
    "TOM", "Tom", "PIEŚŃ", "Pieśń", "AKT", "Akt", "SCENA", "Scena", "LIST", "List", nullptr};
static const char* const kAloneHeads[] = {
    "CONTENTS", "EPILOGUE", "PROLOGUE", "INTRODUCTION", "PREFACE", "APPENDIX", "CONCLUSION",
    "DEDICATION", "FOREWORD", "AFTERWORD", "EPILOG", "PROLOG", "WSTĘP", "Wstęp", "ZAKOŃCZENIE",
    "Zakończenie", "POSŁOWIE", "Posłowie", "PRZEDMOWA", "Przedmowa", nullptr};
static const char* const kOrdinals[] = {
    "THE", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN", "ELEVEN",
    "TWELVE", "FIRST", "SECOND", "THIRD", "FOURTH", "FIFTH", "SIXTH", "SEVENTH", "EIGHTH", "NINTH",
    "TENTH", "LAST", "PIERWSZY", "PIERWSZA", "DRUGI", "DRUGA", "TRZECI", "TRZECIA", nullptr};

// 0 = not a heading, 1 = H1 (chapter), 2 = H2 (section). *roman set for bare numerals.
static int classifyHeading(const char* s, const char* e, bool* roman) {
    *roman = false;
    // strip trailing spaces
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
    const size_t n = (size_t)(e - s);
    if (n == 0 || n > 72) return 0;
    const char last = e[-1];
    if (last == ',' || last == ';') return 0;

    // First token.
    const char* w = s;
    while (w < e && *w != ' ' && *w != '.' && *w != ':' && *w != '\t') ++w;
    const char* rest = w;
    while (rest < e && (*rest == ' ' || *rest == '.' || *rest == ':' || *rest == '\t')) ++rest;

    for (int i = 0; kNumberedHeads[i]; ++i) {
        if (!wordIs(s, w, kNumberedHeads[i])) continue;
        if (rest >= e) return 1;                                   // "CHAPTER" alone
        const char* t = rest;
        while (t < e && *t != ' ' && *t != '.' && *t != ':' && *t != ',') ++t;
        if (isRoman(rest, t) || isDigits(rest, t)) return 1;      // CHAPTER I. / Chapter 12
        for (int j = 0; kOrdinals[j]; ++j) if (wordIs(rest, t, kOrdinals[j])) return 1;
        return 0;                                                  // "Part of me wanted..."
    }
    for (int i = 0; kAloneHeads[i]; ++i) {
        if (wordIs(s, w, kAloneHeads[i]) && rest >= e) return 1;
    }
    // Bare numerals: "I", "II.", "12" -> section, promoted later if the book has nothing better.
    if (isRoman(s, w) || isDigits(s, w)) {
        if (rest >= e) { *roman = true; return 2; }
        // "I. A SCANDAL IN BOHEMIA": numeral, period, title.
        if (w < e && *w == '.' && isRoman(s, w)) return 1;
        if (w < e && *w == '.' && isDigits(s, w) && rest < e && isupper((unsigned char)*rest)) return 1;
        return 0;
    }
    // Short line in capitals: "THE PROPHET", "THE END".
    int letters = 0; bool lower = false, digitOnly = true;
    for (const char* p = s; p < e; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (isalpha(c)) { letters++; digitOnly = false; if (islower(c)) lower = true; }
        else if (c >= 0x80) { letters++; digitOnly = false; }
    }
    if (letters >= 3 && !lower && !digitOnly && last != '.' && n <= 48) return 2;
    return 0;
}

static bool isSeparator(const char* s, const char* e) {
    if (e - s < 3) return false;
    for (const char* p = s; p < e; ++p) {
        if (!strchr("*-_=~ .", *p) && !((unsigned char)*p >= 0x80)) return false;
    }
    // at least three ornament characters
    int marks = 0;
    for (const char* p = s; p < e; ++p) if (*p != ' ') marks++;
    return marks >= 3;
}

static bool looksLikeSubtitle(const LineInfo& L) {
    if (!L.valid || L.blank) return false;
    const char* e = L.e;
    while (e > L.ts && (e[-1] == ' ' || e[-1] == '"' || e[-1] == '\'' || e[-1] == ')' ||
                        (unsigned char)e[-1] == 0x9D || (unsigned char)e[-1] == 0x99)) --e;
    // (the two bytes above end the UTF-8 sequences of ” and ’)
    const size_t n = (size_t)(e - L.ts);
    if (n == 0 || n > 72) return false;
    const char last = e[-1];
    if (strchr(".!?,;:", last)) return false;
    return true;
}

static bool push(Model& m, const Para& p) {
    if (m.count == m.cap) {
        const uint32_t ncap = m.cap ? m.cap * 2 : 512;
        Para* np = (Para*)bigAlloc(m.paras, sizeof(Para) * ncap);
        if (!np) return false;
        m.paras = np;
        m.cap = ncap;
    }
    m.paras[m.count++] = p;
    return true;
}

static uint32_t countWords(const char* s, const char* e) {
    uint32_t n = 0; bool in = false;
    for (const char* p = s; p < e; ++p) {
        const bool sp = (*p == ' ' || *p == '\n' || *p == '\t');
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

bool build(Model& m, const char* text, uint32_t len) {
    release(m);
    m.text = text;
    m.len = len;
    const char* end = text + len;
    // Skip a UTF-8 BOM.
    const char* p = text;
    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;

    int blankRun = 1;                 // start of text counts as a blank before
    bool haveBody = false;
    const char* bodyStart = nullptr;
    const char* bodyEnd = nullptr;
    uint8_t pendingFlags = 0;
    bool lastWasHeading = false;

    auto flagsFor = [&](void) {
        uint8_t f = 0;
        if (blankRun >= 1) f |= F_BLANK_BEFORE;
        if (blankRun >= 2) f |= F_GAP;
        if (lastWasHeading) f |= F_AFTER_HEADING;
        return f;
    };
    auto flushBody = [&](void) {
        if (!haveBody) return;
        Para P{};
        P.start = (uint32_t)(bodyStart - text);
        P.len = (uint32_t)(bodyEnd - bodyStart);
        P.kind = BODY;
        P.flags = pendingFlags;
        m.words += countWords(bodyStart, bodyEnd);
        push(m, P);
        haveBody = false;
        lastWasHeading = false;
    };

    while (p < end) {
        LineInfo L = lineAt(p, end);
        const char* next = nextLineStart(L, end);
        if (L.blank) {
            flushBody();
            blankRun++;
            p = next;
            continue;
        }
        // Illustration placeholders.
        if (L.e - L.ts >= 13 && strncmp(L.ts, "[Illustration", 13) == 0) {
            flushBody();
            const char* q = next;
            if (L.e[-1] != ']') {
                for (int i = 0; i < 6 && q < end; ++i) {
                    LineInfo M = lineAt(q, end);
                    q = nextLineStart(M, end);
                    if (M.e > M.ts && M.e[-1] == ']') break;
                }
            }
            p = q;
            continue;
        }
        if (isSeparator(L.ts, L.e)) {
            flushBody();
            Para P{};
            P.start = (uint32_t)(L.ts - text); P.len = (uint32_t)(L.e - L.ts);
            P.kind = SEPARATOR; P.flags = flagsFor();
            push(m, P);
            blankRun = 0;
            lastWasHeading = false;
            p = next;
            continue;
        }
        // Heading: isolated by blank lines (or followed by a subtitle and then a blank).
        if (blankRun >= 1 && !haveBody) {
            bool roman = false;
            const int h = classifyHeading(L.ts, L.e, &roman);
            if (h) {
                LineInfo N = lineAt(next, end);
                LineInfo NN = lineAt(nextLineStart(N, end), end);
                const bool nextBlank = !N.valid || N.blank;
                const bool sub = !nextBlank && looksLikeSubtitle(N) && (!NN.valid || NN.blank);
                if (nextBlank || sub) {
                    Para P{};
                    P.start = (uint32_t)(L.ts - text); P.len = (uint32_t)(L.e - L.ts);
                    P.kind = h == 1 ? H1 : H2;
                    P.flags = flagsFor() | (roman ? F_ROMAN : 0);
                    push(m, P);
                    lastWasHeading = true;
                    p = next;
                    if (sub) {
                        Para S{};
                        S.start = (uint32_t)(N.ts - text); S.len = (uint32_t)(N.e - N.ts);
                        S.kind = SUBTITLE; S.flags = F_AFTER_HEADING;
                        push(m, S);
                        p = nextLineStart(N, end);
                    }
                    blankRun = 0;
                    continue;
                }
            }
        }
        // Verse: indented, and the next line is blank, indented or the end.
        if (L.indent >= 1) {
            LineInfo N = lineAt(next, end);
            const bool verse = !N.valid || N.blank || N.indent >= 1 || haveBody;
            if (verse && !haveBody) {
                Para P{};
                P.start = (uint32_t)(L.ts - text); P.len = (uint32_t)(L.e - L.ts);
                P.kind = VERSE; P.indent = (uint8_t)(L.indent > 40 ? 40 : L.indent);
                P.flags = flagsFor();
                m.words += countWords(L.ts, L.e);
                push(m, P);
                blankRun = 0;
                lastWasHeading = false;
                p = next;
                continue;
            }
        }
        // Body text: start or extend the current paragraph.
        if (!haveBody) {
            haveBody = true;
            bodyStart = L.ts;
            pendingFlags = flagsFor();
        }
        bodyEnd = L.e;
        blankRun = 0;
        p = next;
    }
    flushBody();

    // A book with only bare numerals for headings ("I", "II", "III") uses them
    // as chapters. A book with real chapter lines keeps numerals as sections.
    uint32_t h1 = 0, romanH2 = 0;
    for (uint32_t i = 0; i < m.count; ++i) {
        if (m.paras[i].kind == H1) h1++;
        else if (m.paras[i].kind == H2 && (m.paras[i].flags & F_ROMAN)) romanH2++;
    }
    if (romanH2 >= 2 && h1 * 2 < romanH2) {
        for (uint32_t i = 0; i < m.count; ++i)
            if (m.paras[i].kind == H2 && (m.paras[i].flags & F_ROMAN)) m.paras[i].kind = H1;
        h1 = romanH2;
    }
    // Chapter index, skipping tables of contents.
    if (h1) {
        m.chapters = (uint32_t*)bigAlloc(nullptr, sizeof(uint32_t) * h1);
        if (m.chapters) {
            for (uint32_t i = 0; i < m.count; ++i) {
                const Para& P = m.paras[i];
                if (P.kind != H1) continue;
                const char* s = text + P.start;
                const char* e = s + P.len;
                const char* w = s;
                while (w < e && *w != ' ' && *w != '.' && *w != ':') ++w;
                if (wordIs(s, w, "CONTENTS") || wordIs(s, w, "SPIS")) continue;
                m.chapters[m.chapterCount++] = i;
            }
        }
    }
    return m.count > 0;
}

void release(Model& m) {
    if (m.paras) bigFree(m.paras);
    if (m.chapters) bigFree(m.chapters);
    m.paras = nullptr; m.chapters = nullptr;
    m.count = m.cap = m.chapterCount = 0;
    m.words = 0;
    m.text = nullptr; m.len = 0;
}

uint32_t paraForOffset(const Model& m, uint32_t abs) {
    if (m.count == 0) return 0;
    uint32_t lo = 0, hi = m.count - 1;
    while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        if (m.paras[mid].start <= abs) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void copyRange(char* buf, size_t cap, const char* s, const char* e) {
    size_t n = (size_t)(e - s);
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
}

void chapterTitle(const Model& m, uint32_t chapter, char* buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = 0;
    if (chapter >= m.chapterCount) return;
    const uint32_t i = m.chapters[chapter];
    const Para& P = m.paras[i];
    const char* s = m.text + P.start;
    const char* e = s + P.len;
    while (e > s && (e[-1] == ' ' || e[-1] == '.')) --e;   // "CHAPTER I." -> "CHAPTER I"
    // Collapse runs of spaces (Gutenberg aligns "STAVE I:  MARLEY'S GHOST").
    size_t n = 0; bool sp = false;
    for (const char* p = s; p < e && n + 1 < cap; ++p) {
        if (*p == ' ') { if (!sp) buf[n++] = ' '; sp = true; }
        else { buf[n++] = *p; sp = false; }
    }
    buf[n] = 0;
    if (i + 1 < m.count && m.paras[i + 1].kind == SUBTITLE && n + 4 < cap) {
        const Para& S = m.paras[i + 1];
        const char* ss = m.text + S.start;
        const char* se = ss + S.len;
        const char* sep = " \xC2\xB7 ";   // " · "
        const size_t sl = strlen(sep);
        if (n + sl < cap) {
            memcpy(buf + n, sep, sl); n += sl;
            copyRange(buf + n, cap - n, ss, se);
        }
    }
}

int chapterForOffset(const Model& m, uint32_t abs) {
    int found = -1;
    for (uint32_t c = 0; c < m.chapterCount; ++c) {
        if (m.paras[m.chapters[c]].start <= abs) found = (int)c; else break;
    }
    return found;
}

}  // namespace text
