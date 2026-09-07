#pragma once
#include <stdint.h>

// Decode one UTF-8 sequence at p (p < end), advance p. Malformed input yields
// U+FFFD and consumes one byte, so a scan always terminates.
static inline uint32_t utf8Next(const char*& p, const char* end) {
    const uint8_t c = (uint8_t)*p++;
    if (c < 0x80) return c;
    int n; uint32_t cp;
    if ((c & 0xE0) == 0xC0)      { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else return 0xFFFD;
    for (int i = 0; i < n; ++i) {
        if (p >= end || ((uint8_t)*p & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
    }
    return cp;
}

// Characters that occupy no space and must not be drawn.
static inline bool utf8Invisible(uint32_t cp) {
    return cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || cp == 0x2060;
}

// Encode cp into out (>= 4 bytes); returns the byte count.
static inline int utf8Put(char* out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
