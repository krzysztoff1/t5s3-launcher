#include "font.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "display.h"
#include "utf8.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Embedded faces (board_build.embed_files in platformio.ini).
extern "C" {
extern const uint8_t _binary_assets_fonts_LiterataR_ttf_start[] asm("_binary_assets_fonts_LiterataR_ttf_start");
extern const uint8_t _binary_assets_fonts_LiterataI_ttf_start[] asm("_binary_assets_fonts_LiterataI_ttf_start");
extern const uint8_t _binary_assets_fonts_LiterataB_ttf_start[] asm("_binary_assets_fonts_LiterataB_ttf_start");
extern const uint8_t _binary_assets_fonts_InterR_ttf_start[]    asm("_binary_assets_fonts_InterR_ttf_start");
extern const uint8_t _binary_assets_fonts_InterSB_ttf_start[]   asm("_binary_assets_fonts_InterSB_ttf_start");
}

namespace font {

struct FaceInfo {
    stbtt_fontinfo info;
    int asc = 0, desc = 0, gap = 0;
    bool ok = false;
};
static FaceInfo g_face[FACE_COUNT];

struct Glyph {
    uint64_t key;        // 0 = empty
    float    adv;
    int16_t  x0, y0;     // bitmap origin relative to the pen (y down)
    uint16_t w, h;
    int16_t  gi;         // glyph index
    uint8_t  hasBmp;     // rasterised (bmp may still be null for blank glyphs)
    uint8_t* bmp;        // w*h levels 0..3, PSRAM
};
static const uint32_t GLYPH_CAP = 8192;              // power of two
static Glyph*   g_glyphs = nullptr;
static uint32_t g_glyphCount = 0;
static size_t   g_bmpBytes = 0;

struct KernEntry { uint64_t key; int32_t adv; };     // adv in font units
static const uint32_t KERN_CAP = 8192;
static KernEntry* g_kern = nullptr;
static uint32_t   g_kernCount = 0;

static uint8_t g_t3 = 140, g_t2 = 75, g_t1 = 28;

// ASCII fast path: (face, size) slots holding pointers for codepoints 32..127.
struct SizeSlot { uint64_t tag; uint32_t lastUse; Glyph* ascii[96]; };
static SizeSlot g_slots[10];
static uint32_t g_useClock = 0;

static inline uint32_t sizeKey(float px) { return (uint32_t)lroundf(px * 4.0f); }
static inline float    keyPx(uint32_t k) { return k * 0.25f; }
static inline uint64_t glyphKey(Face f, uint32_t sk, uint32_t cp) {
    return ((uint64_t)(f + 1) << 56) | ((uint64_t)sk << 32) | cp;
}
static inline uint32_t hash64(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33;
    return (uint32_t)k;
}
static inline float scaleFor(Face f, uint32_t sk) {
    return stbtt_ScaleForMappingEmToPixels(&g_face[f].info, keyPx(sk));
}

bool begin() {
    const uint8_t* blobs[FACE_COUNT] = {
        _binary_assets_fonts_LiterataR_ttf_start, _binary_assets_fonts_LiterataI_ttf_start,
        _binary_assets_fonts_LiterataB_ttf_start, _binary_assets_fonts_InterR_ttf_start,
        _binary_assets_fonts_InterSB_ttf_start,
    };
    static const char* names[FACE_COUNT] = {"LiterataR", "LiterataI", "LiterataB", "InterR", "InterSB"};
    bool all = true;
    for (int i = 0; i < FACE_COUNT; ++i) {
        FaceInfo& fi = g_face[i];
        fi.ok = stbtt_InitFont(&fi.info, blobs[i], stbtt_GetFontOffsetForIndex(blobs[i], 0)) != 0;
        if (fi.ok) stbtt_GetFontVMetrics(&fi.info, &fi.asc, &fi.desc, &fi.gap);
        else { Serial.printf("[font] %s failed to parse\n", names[i]); all = false; }
    }
    if (!g_glyphs) g_glyphs = (Glyph*)heap_caps_calloc(GLYPH_CAP, sizeof(Glyph), MALLOC_CAP_SPIRAM);
    if (!g_kern)   g_kern = (KernEntry*)heap_caps_calloc(KERN_CAP, sizeof(KernEntry), MALLOC_CAP_SPIRAM);
    memset(g_slots, 0, sizeof g_slots);
    if (!g_glyphs || !g_kern) { Serial.println("[font] cache alloc failed"); return false; }
    Serial.printf("[font] %d faces ready\n", FACE_COUNT);
    return all;
}

void setThresholds(uint8_t t3, uint8_t t2, uint8_t t1) {
    g_t3 = t3; g_t2 = t2; g_t1 = t1;
    flushCache();
}

void flushCache() {
    if (!g_glyphs) return;
    for (uint32_t i = 0; i < GLYPH_CAP; ++i) {
        if (g_glyphs[i].bmp) heap_caps_free(g_glyphs[i].bmp);
    }
    memset(g_glyphs, 0, sizeof(Glyph) * GLYPH_CAP);
    memset(g_slots, 0, sizeof g_slots);
    g_glyphCount = 0;
    g_bmpBytes = 0;
}

size_t cacheBytes() { return g_bmpBytes + (g_glyphs ? sizeof(Glyph) * GLYPH_CAP : 0); }
size_t cacheGlyphs() { return g_glyphCount; }

// Metrics-only entry for (face, size, cp); creates it on a miss.
static Glyph* lookupSlow(Face f, uint32_t sk, uint32_t cp) {
    const uint64_t key = glyphKey(f, sk, cp);
    uint32_t i = hash64(key) & (GLYPH_CAP - 1);
    for (;;) {
        Glyph& g = g_glyphs[i];
        if (g.key == key) return &g;
        if (g.key == 0) break;
        i = (i + 1) & (GLYPH_CAP - 1);
    }
    if (g_glyphCount + 1 > GLYPH_CAP * 3 / 4) {
        flushCache();
        i = hash64(key) & (GLYPH_CAP - 1);
    }
    Glyph& g = g_glyphs[i];
    memset(&g, 0, sizeof g);
    g.key = key;
    const FaceInfo& fi = g_face[f];
    if (!fi.ok) { g.gi = -1; g_glyphCount++; return &g; }
    int gi = stbtt_FindGlyphIndex(&fi.info, (int)cp);
    if (gi == 0 && cp == 0xA0) gi = stbtt_FindGlyphIndex(&fi.info, ' ');
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&fi.info, gi, &adv, &lsb);
    g.adv = adv * scaleFor(f, sk);
    g.gi = (int16_t)gi;
    g_glyphCount++;
    return &g;
}

static Glyph* lookup(Face f, uint32_t sk, uint32_t cp) {
    if (!g_glyphs) return nullptr;
    if (cp >= 32 && cp < 128) {
        const uint64_t tag = ((uint64_t)(f + 1) << 32) | sk;
        SizeSlot* slot = nullptr;
        SizeSlot* victim = &g_slots[0];
        for (auto& s : g_slots) {
            if (s.tag == tag) { slot = &s; break; }
            if (s.lastUse < victim->lastUse) victim = &s;
        }
        if (!slot) {
            slot = victim;
            memset(slot, 0, sizeof *slot);
            slot->tag = tag;
        }
        slot->lastUse = ++g_useClock;
        Glyph* g = slot->ascii[cp - 32];
        if (g && g->key == glyphKey(f, sk, cp)) return g;
        g = lookupSlow(f, sk, cp);
        // lookupSlow may have flushed, which zeroed the slots; re-find ours.
        for (auto& s : g_slots) if (s.tag == tag) { s.ascii[cp - 32] = g; break; }
        if (g_slots[0].tag == 0 && slot->tag == 0) { slot->tag = tag; slot->lastUse = ++g_useClock; slot->ascii[cp - 32] = g; }
        return g;
    }
    return lookupSlow(f, sk, cp);
}

static void rasterize(Face f, uint32_t sk, Glyph& g) {
    g.hasBmp = 1;
    if (g.gi < 0) return;
    const FaceInfo& fi = g_face[f];
    const float scale = scaleFor(f, sk);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&fi.info, g.gi, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 600 || h > 600) return;
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    stbtt_MakeGlyphBitmap(&fi.info, buf, w, h, w, scale, scale, g.gi);
    const int n = w * h;
    for (int i = 0; i < n; ++i) {
        uint32_t c = buf[i];
        c += c >> 2;                       // mild stem darkening: 4 levels are unkind to hairlines
        buf[i] = c >= g_t3 ? 3 : c >= g_t2 ? 2 : c >= g_t1 ? 1 : 0;
    }
    g.bmp = buf; g.w = (uint16_t)w; g.h = (uint16_t)h; g.x0 = (int16_t)x0; g.y0 = (int16_t)y0;
    g_bmpBytes += (size_t)n;
}

static int kernUnits(Face f, int g1, int g2) {
    if (g1 <= 0 || g2 <= 0 || !g_kern) return 0;
    const uint64_t key = (1ULL << 63) | ((uint64_t)(f + 1) << 48) | ((uint64_t)(uint32_t)g1 << 24) | (uint32_t)g2;
    uint32_t i = hash64(key) & (KERN_CAP - 1);
    for (;;) {
        KernEntry& e = g_kern[i];
        if (e.key == key) return e.adv;
        if (e.key == 0) break;
        i = (i + 1) & (KERN_CAP - 1);
    }
    if (g_kernCount + 1 > KERN_CAP * 3 / 4) {
        memset(g_kern, 0, sizeof(KernEntry) * KERN_CAP);
        g_kernCount = 0;
        i = hash64(key) & (KERN_CAP - 1);
    }
    const int adv = stbtt_GetGlyphKernAdvance(&g_face[f].info, g1, g2);
    g_kern[i].key = key;
    g_kern[i].adv = adv;
    g_kernCount++;
    return adv;
}

static void blit(int px, int py, const Glyph& g, uint8_t ink) {
    uint8_t* fb = display::fb();
    int xs = 0, xe = g.w;
    if (px < 0) xs = -px;
    if (px + xe > display::W) xe = display::W - px;
    if (xs >= xe) return;
    for (int r = 0; r < g.h; ++r) {
        const int y = py + r;
        if (y < 0 || y >= display::H) continue;
        const uint8_t* src = g.bmp + r * g.w;
        uint8_t* dst = fb + (size_t)y * display::W + px;
        if (ink == 3) {
            for (int x = xs; x < xe; ++x) { const uint8_t l = src[x]; if (l > dst[x]) dst[x] = l; }
        } else if (ink == 0) {
            for (int x = xs; x < xe; ++x) { const uint8_t l = 3 - src[x]; if (l < dst[x]) dst[x] = l; }
        } else {
            for (int x = xs; x < xe; ++x) { uint8_t l = src[x]; if (l > ink) l = ink; if (l > dst[x]) dst[x] = l; }
        }
    }
}

float ascent(Face f, float px) {
    const uint32_t sk = sizeKey(px);
    return g_face[f].asc * scaleFor(f, sk);
}
float descent(Face f, float px) {
    const uint32_t sk = sizeKey(px);
    return -g_face[f].desc * scaleFor(f, sk);
}
float lineHeight(Face f, float px) {
    const uint32_t sk = sizeKey(px);
    return (g_face[f].asc - g_face[f].desc + g_face[f].gap) * scaleFor(f, sk);
}

float advance(Face f, float px, uint32_t cp) {
    Glyph* g = lookup(f, sizeKey(px), cp);
    return g ? g->adv : 0.f;
}

float kern(Face f, float px, uint32_t a, uint32_t b) {
    const uint32_t sk = sizeKey(px);
    Glyph* ga = lookup(f, sk, a);
    const int gia = ga ? ga->gi : -1;
    Glyph* gb = lookup(f, sk, b);
    const int gib = gb ? gb->gi : -1;
    return kernUnits(f, gia, gib) * scaleFor(f, sk);
}

// Pen-driven primitives shared by measuring and drawing so both agree exactly.
float glyphAdvance(Face f, float px, uint32_t cp, int& prevGi) {
    const uint32_t sk = sizeKey(px);
    Glyph* g = lookup(f, sk, cp);
    if (!g) return 0.f;
    float adv = g->adv;
    if (prevGi > 0 && g->gi > 0) adv += kernUnits(f, prevGi, g->gi) * scaleFor(f, sk);
    prevGi = g->gi;
    return adv;
}

float drawGlyph(float penX, int y, Face f, float px, uint32_t cp, int& prevGi, uint8_t ink) {
    const uint32_t sk = sizeKey(px);
    Glyph* g = lookup(f, sk, cp);
    if (!g) return 0.f;
    float k = 0.f;
    if (prevGi > 0 && g->gi > 0) k = kernUnits(f, prevGi, g->gi) * scaleFor(f, sk);
    if (!g->hasBmp) rasterize(f, sk, *g);
    if (g->bmp) blit((int)lroundf(penX + k) + g->x0, y + g->y0, *g, ink);
    prevGi = g->gi;
    return g->adv + k;
}

float width(const char* s, const char* e, Face f, float px) {
    float w = 0.f;
    int prev = -1;
    while (s < e) {
        const uint32_t cp = utf8Next(s, e);
        if (utf8Invisible(cp)) continue;
        w += glyphAdvance(f, px, cp, prev);
    }
    return w;
}

float draw(int x, int y, const char* s, const char* e, Face f, float px, uint8_t ink) {
    float pen = (float)x;
    int prev = -1;
    while (s < e) {
        const uint32_t cp = utf8Next(s, e);
        if (utf8Invisible(cp)) continue;
        pen += drawGlyph(pen, y, f, px, cp, prev, ink);
    }
    return pen - x;
}

float draw(int x, int y, const char* s, Face f, float px, uint8_t ink) {
    return draw(x, y, s, s + strlen(s), f, px, ink);
}

float drawCentered(int cx, int y, const char* s, Face f, float px, uint8_t ink) {
    const float w = width(s, f, px);
    return draw((int)lroundf(cx - w / 2), y, s, f, px, ink);
}

float drawRight(int rx, int y, const char* s, Face f, float px, uint8_t ink) {
    const float w = width(s, f, px);
    return draw((int)lroundf(rx - w), y, s, f, px, ink);
}

float drawFit(int x, int y, const char* s, Face f, float px, int maxW, uint8_t ink) {
    const char* e = s + strlen(s);
    if (width(s, e, f, px) <= maxW) return draw(x, y, s, e, f, px, ink);
    static const char* ell = "\xE2\x80\xA6";   // U+2026
    const float ellW = width(ell, f, px);
    // Longest prefix (by codepoint) that fits with the ellipsis.
    const char* p = s;
    const char* best = s;
    while (p < e) {
        const char* q = p;
        utf8Next(q, e);
        if (width(s, q, f, px) + ellW > maxW) break;
        best = q;
        p = q;
    }
    // Do not leave a dangling space before the ellipsis.
    while (best > s && (best[-1] == ' ')) --best;
    float w = draw(x, y, s, best, f, px, ink);
    w += draw((int)lroundf(x + w), y, ell, f, px, ink);
    return w;
}

int drawWrapped(int x, int y, int w, const char* s, Face f, float px, int lineH, int maxLines,
                uint8_t ink, int align) {
    const char* e = s + strlen(s);
    const char* p = s;
    int lines = 0;
    while (p < e && lines < maxLines) {
        while (p < e && *p == ' ') ++p;
        if (p >= e) break;
        const char* lineStart = p;
        const char* lastFit = nullptr;
        const char* q = p;
        while (q < e) {
            const char* wordEnd = q;
            while (wordEnd < e && *wordEnd != ' ' && *wordEnd != '\n') ++wordEnd;
            if (width(lineStart, wordEnd, f, px) > w) break;
            lastFit = wordEnd;
            if (wordEnd < e && *wordEnd == '\n') break;
            q = wordEnd;
            while (q < e && *q == ' ') ++q;
        }
        if (!lastFit) {                       // one word wider than the box
            lastFit = lineStart;
            while (lastFit < e && *lastFit != ' ' && *lastFit != '\n') ++lastFit;
        }
        const bool more = (lastFit < e) && (lines + 1 == maxLines);
        const float lw = width(lineStart, lastFit, f, px);
        const int lx = align == 1 ? (int)lroundf(x + (w - lw) / 2) : x;
        if (more) drawFit(lx, y, lineStart, f, px, w, ink);
        else draw(lx, y, lineStart, lastFit, f, px, ink);
        y += lineH;
        lines++;
        p = lastFit;
        if (p < e && *p == '\n') ++p;
    }
    return lines;
}

float fitSize(const char* s, Face f, int w, float minPx, float maxPx) {
    for (float px = maxPx; px > minPx; px -= 1.f) {
        if (width(s, f, px) <= w) return px;
    }
    return minPx;
}

}  // namespace font
