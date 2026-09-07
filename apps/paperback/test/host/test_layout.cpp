// Runs the paragraph model and the paginator over every sample book and checks
// the invariants the reader relies on. Build: test/host/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "layout.h"
#include "textmodel.h"

namespace font { extern std::string* g_sink; }

static char* slurp(const char* path, uint32_t& len) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); len = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(len + 1);
    fread(buf, 1, len, f); buf[len] = 0; fclose(f);
    return buf;
}

static const char* kindName(uint8_t k) {
    static const char* n[] = {"BODY", "VERSE", "H1", "H2", "SUB", "SEP"};
    return k < 6 ? n[k] : "?";
}

// Words of a text range, whitespace-normalised, underscores dropped at word edges.
static void wordsOf(const char* s, const char* e, std::vector<std::string>& out) {
    std::string cur;
    for (const char* p = s; p < e; ++p) {
        const char c = *p;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    for (int a = 1; a < argc; ++a) {
        uint32_t len; char* txt = slurp(argv[a], len);
        if (!txt) { printf("cannot read %s\n", argv[a]); return 2; }
        text::Model m;
        text::build(m, txt, len);
        int kinds[6] = {0};
        for (uint32_t i = 0; i < m.count; ++i) if (m.paras[i].kind < 6) kinds[m.paras[i].kind]++;
        printf("== %s: %u bytes, %u paras (body %d verse %d h1 %d h2 %d sub %d sep %d), ~%u words, %u chapters\n",
               argv[a], len, m.count, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], m.words, m.chapterCount);
        for (uint32_t c = 0; c < m.chapterCount && c < 40; ++c) {
            char t[140]; text::chapterTitle(m, c, t, sizeof t);
            printf("   ch %2u: %s\n", c + 1, t);
        }
        if (m.chapterCount > 40) printf("   ... %u more\n", m.chapterCount - 40);
        // Show a few H2 / verse samples for eyeballing.
        int shown = 0;
        for (uint32_t i = 0; i < m.count && shown < 6; ++i) {
            if (m.paras[i].kind == text::H2 || m.paras[i].kind == text::SEPARATOR) {
                printf("   %s: %.*s\n", kindName(m.paras[i].kind), (int)(m.paras[i].len > 60 ? 60 : m.paras[i].len), txt + m.paras[i].start);
                shown++;
            }
        }

        layout::Style st{font::SERIF, font::SERIF_ITALIC, font::SERIF_BOLD, 25.f, 36, 32, 476, 58, 914, true, 37, 0};
        layout::Pagination pg;
        layout::paginate(m, st, pg);
        printf("   pages: %u\n", pg.count);

        // Invariants: offsets increase; composing page i yields page i+1's start; words are preserved.
        layout::Page* page = new layout::Page;
        uint32_t prevOff = 0;
        std::vector<std::string> laidOut;
        for (uint32_t i = 0; i < pg.count; ++i) {
            const uint32_t off = layout::absOffset(m, pg.pages[i]);
            if (i && off <= prevOff) { printf("   FAIL page %u offset %u <= %u\n", i, off, prevOff); failures++; }
            prevOff = off;
            layout::compose(m, st, pg.pages[i], *page);
            if (page->count == 0 && !page->last) { printf("   FAIL page %u is empty\n", i); failures++; }
            if (page->count > 40) { printf("   FAIL page %u has %d lines\n", i, page->count); failures++; }
            if (i + 1 < pg.count) {
                const layout::Pos& n = pg.pages[i + 1];
                if (page->next.para != n.para || page->next.off != n.off) {
                    printf("   FAIL page %u next=(%u,%u) but page %u starts at (%u,%u)\n", i, page->next.para, page->next.off, i + 1, n.para, n.off);
                    failures++;
                }
            }
            for (int l = 0; l < page->count; ++l) {
                const layout::Line& L = page->lines[l];
                if (L.kind == text::SEPARATOR) continue;
                if (L.e < L.s || L.e > len) { printf("   FAIL bad line range\n"); failures++; continue; }
                wordsOf(txt + L.s, txt + L.e, laidOut);
                if (L.x < st.colX - 1 || L.x > st.colX + st.colW) { printf("   FAIL line x=%d\n", L.x); failures++; }
                if (L.y < st.top || L.y > st.bottom + 10) { printf("   FAIL line y=%d\n", L.y); failures++; }
            }
        }
        // Every word of every laid-out paragraph must appear exactly once, in order.
        // Words wider than the column are split across lines, so compare the
        // concatenation rather than the count.
        std::vector<std::string> expected;
        for (uint32_t i = 0; i < m.count; ++i) {
            if (m.paras[i].kind == text::SEPARATOR) continue;
            wordsOf(txt + m.paras[i].start, txt + m.paras[i].start + m.paras[i].len, expected);
        }
        std::string joinedA, joinedB;
        for (auto& w : expected) joinedA += w;
        for (auto& w : laidOut) joinedB += w;
        if (joinedA != joinedB) {
            size_t k = 0;
            while (k < joinedA.size() && k < joinedB.size() && joinedA[k] == joinedB[k]) k++;
            printf("   FAIL text differs at byte %zu: model '%.40s' vs laid out '%.40s'\n", k, joinedA.c_str() + k, joinedB.c_str() + k);
            failures++;
        }
        printf("   words: %zu (laid out in %zu pieces)\n", expected.size(), laidOut.size());
        // Page-for-offset round trip.
        for (uint32_t i = 0; i < pg.count; i += 7) {
            const uint32_t off = layout::absOffset(m, pg.pages[i]);
            if (layout::pageForOffset(pg, m, off) != i) { printf("   FAIL pageForOffset(%u) != %u\n", off, i); failures++; }
            if (i + 1 < pg.count && layout::pageForOffset(pg, m, off + 1) != i) { printf("   FAIL pageForOffset(%u+1) != %u\n", off, i); failures++; }
        }
        delete page;
        layout::release(pg);
        text::release(m);
        free(txt);
    }
    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
