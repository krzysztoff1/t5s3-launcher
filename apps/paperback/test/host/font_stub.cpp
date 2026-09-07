// Host stand-in for font.cpp: fixed-width metrics so the layout engine can be
// exercised without a panel. Widths are deterministic per codepoint.
#include "font.h"
#include <string.h>
#include <string>
#include <vector>

namespace display { void fillCircle(int, int, int, uint8_t) {} }

namespace font {
static float w(uint32_t cp, float px) {
    if (cp == ' ' || cp == 0xA0) return px * 0.25f;
    if (cp == 'i' || cp == 'l' || cp == '.' || cp == ',' || cp == '\'' || cp == 0x2019) return px * 0.28f;
    if (cp == 'm' || cp == 'w' || cp == 'M' || cp == 'W') return px * 0.8f;
    if (cp >= 'A' && cp <= 'Z') return px * 0.65f;
    return px * 0.5f;
}
bool begin() { return true; }
float ascent(Face, float px) { return px * 0.9f; }
float descent(Face, float px) { return px * 0.25f; }
float lineHeight(Face, float px) { return px * 1.2f; }
float advance(Face, float px, uint32_t cp) { return w(cp, px); }
float kern(Face, float, uint32_t, uint32_t) { return 0.f; }
float glyphAdvance(Face, float px, uint32_t cp, int& prevGi) { prevGi = (int)cp; return w(cp, px); }
std::string* g_sink = nullptr;
float drawGlyph(float, int, Face f, float px, uint32_t cp, int& prevGi, uint8_t) {
    if (g_sink) { char t[5] = {0}; if (cp < 0x80) t[0] = (char)cp; else { t[0] = '?'; } g_sink->append(t); if (f == SERIF_ITALIC) g_sink->append(""); }
    prevGi = (int)cp; return w(cp, px);
}
float width(const char* s, const char* e, Face f, float px) { float t = 0; int p = -1; while (s < e) { uint32_t cp = (uint8_t)*s++; if (cp >= 0x80) { while (s < e && ((uint8_t)*s & 0xC0) == 0x80) s++; } t += glyphAdvance(f, px, cp, p); } return t; }
float draw(int, int, const char*, const char*, Face, float, uint8_t) { return 0; }
float draw(int, int, const char*, Face, float, uint8_t) { return 0; }
float drawCentered(int, int, const char*, Face, float, uint8_t) { return 0; }
float drawRight(int, int, const char*, Face, float, uint8_t) { return 0; }
float drawFit(int, int, const char*, Face, float, int, uint8_t) { return 0; }
int drawWrapped(int, int, int, const char*, Face, float, int, int, uint8_t, int) { return 0; }
float fitSize(const char*, Face, int, float, float maxPx) { return maxPx; }
void setThresholds(uint8_t, uint8_t, uint8_t) {}
void flushCache() {}
size_t cacheBytes() { return 0; }
size_t cacheGlyphs() { return 0; }
}
