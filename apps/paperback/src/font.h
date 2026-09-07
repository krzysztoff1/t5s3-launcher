#pragma once
// Vector text for the framebuffer: five embedded TrueType faces rasterised on
// demand by stb_truetype at whatever pixel size a screen asks for, quantised to
// the panel's four levels and cached in PSRAM. Coordinates are canvas pixels;
// y is always the BASELINE.
#include <stddef.h>
#include <stdint.h>

namespace font {

enum Face : uint8_t { SERIF = 0, SERIF_ITALIC, SERIF_BOLD, SANS, SANS_BOLD, FACE_COUNT };

bool begin();                                       // parse the embedded fonts

float ascent(Face f, float px);                     // above the baseline, px
float descent(Face f, float px);                    // below the baseline, px (positive)
float lineHeight(Face f, float px);                 // natural line advance, px

float advance(Face f, float px, uint32_t cp);       // horizontal advance of one glyph
// Kerning between two codepoints, px (negative pulls together). 0 across faces.
float kern(Face f, float px, uint32_t a, uint32_t b);

// Width of a UTF-8 run [s, e), kerning included.
float width(const char* s, const char* e, Face f, float px);
inline float width(const char* s, Face f, float px);

// Draw a UTF-8 run with the pen at (x, y). ink: 3 = black ... 1 = light grey,
// 0 = white (for text on dark fills). Returns the advance.
float draw(int x, int y, const char* s, const char* e, Face f, float px, uint8_t ink = 3);
float draw(int x, int y, const char* s, Face f, float px, uint8_t ink = 3);
float drawCentered(int cx, int y, const char* s, Face f, float px, uint8_t ink = 3);
float drawRight(int rx, int y, const char* s, Face f, float px, uint8_t ink = 3);
// Draw truncated with an ellipsis so it fits maxW. Returns the drawn width.
float drawFit(int x, int y, const char* s, Face f, float px, int maxW, uint8_t ink = 3);
// Word-wrap into at most maxLines lines of width w (the last one gets an
// ellipsis if text remains). align: 0 left, 1 centre. Returns lines drawn.
int drawWrapped(int x, int y, int w, const char* s, Face f, float px, int lineH, int maxLines,
                uint8_t ink = 3, int align = 0);
// Largest px <= maxPx at which `s` fits in w on one line (>= minPx).
float fitSize(const char* s, Face f, int w, float minPx, float maxPx);

// Coverage thresholds for levels 3/2/1 (0..255); tune from the console.
void setThresholds(uint8_t t3, uint8_t t2, uint8_t t1);
void flushCache();                                  // drop every cached glyph
size_t cacheBytes();
size_t cacheGlyphs();

}  // namespace font

inline float font::width(const char* s, Face f, float px) {
    const char* e = s;
    while (*e) ++e;
    return width(s, e, f, px);
}
