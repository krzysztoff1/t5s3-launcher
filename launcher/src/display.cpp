#include "display.h"

#include <Arduino.h>
#include "hw.h"
#include <Wire.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"

#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

namespace display {

const GFXfont* const FONT_HERO  = &FreeSansBold24pt7b;
const GFXfont* const FONT_TITLE = &FreeSansBold18pt7b;
const GFXfont* const FONT_ITEM  = &FreeSansBold12pt7b;
const GFXfont* const FONT_BODY  = &FreeSans12pt7b;
const GFXfont* const FONT_LABEL = &FreeSansBold9pt7b;
const GFXfont* const FONT_SMALL = &FreeSans9pt7b;

// GFXcanvas8 with a caller-supplied (PSRAM) buffer. Its own constructor would
// malloc 518 KB from internal RAM, which does not exist.
class Canvas : public GFXcanvas8 {
public:
    Canvas() : GFXcanvas8(W, H, false) {}
    void attach(uint8_t* buf) { buffer = buf; }
};

static Canvas       g_canvas;
static uint8_t*     g_fb = nullptr;
static EPD_Painter* g_painter = nullptr;
static bool         g_ready = false;

bool ready() { return g_ready; }
GFXcanvas8& gfx() { return g_canvas; }

bool begin() {
    if (g_ready) return true;

    g_fb = (uint8_t*)heap_caps_aligned_alloc(16, (size_t)W * H, MALLOC_CAP_SPIRAM);
    if (!g_fb) {
        Serial.println("[display] PSRAM framebuffer alloc failed");
        return false;
    }
    memset(g_fb, WHITE, (size_t)W * H);
    g_canvas.attach(g_fb);

    // Everything below follows OpenTrailPaper's epd_compat.cpp, which paid for
    // these lessons on this exact board:
    //  * hand the driver OUR Wire, or its own second TwoWire(0) comes up with no
    //    buffers and powerctl init fails fatally;
    //  * auto-shutdown must be off, or begin() may never return;
    //  * make the driver put its 129,600-byte fast buffer in PSRAM (by making
    //    a big internal block unavailable during begin()), or Wi-Fi/BLE will
    //    not fit next to it afterwards.
    EPD_Painter::Config cfg = EPD_LILYGO_T5_S3_GPS_PRESET;
    cfg.i2c.wire = &Wire;
    static EPD_Painter painter(cfg, /*portrait=*/true);
    painter.setAutoShutdown(false);

    const size_t fastBytes = (size_t)960 * 540 / 4;
    const size_t keepFree  = 48 * 1024;
    void* ballast = nullptr;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest > fastBytes + keepFree) {
        ballast = heap_caps_malloc(largest - keepFree, MALLOC_CAP_INTERNAL);
    }
    Serial.printf("[display] internal free before begin: %u (largest %u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const bool ok = painter.begin();
    if (ballast) heap_caps_free(ballast);
    if (!ok) {
        Serial.printf("[display] EPD_Painter::begin() failed; internal=%u largest=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    g_painter = &painter;
    g_ready = true;
    hw::rearmSideButton();   // the driver just took the button's expander pin
    Serial.printf("[display] ready, %d grey levels, internal free %u\n",
                  painter.greyLevels(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // E-paper keeps whatever the previous firmware left on the glass, and a
    // first paint only drives the pixels that differ from the driver's idea of
    // "blank". Scrub before drawing anything, as OpenTrailPaper does at boot.
    hardClear(3);
    return true;
}

static void waitIdle(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (g_painter && !g_painter->paintIdle() && millis() - t0 < timeoutMs) delay(5);
}

void paint() {
    if (!g_ready) return;
    g_painter->paint(g_fb);
    waitIdle(5000);
}

void hardClear(int passes) {
    if (!g_ready) return;
    for (int i = 0; i < passes; ++i) {
        g_painter->clear();
        waitIdle(5000);
        delay(100);
    }
    memset(g_fb, WHITE, (size_t)W * H);
}

void end() {
    if (!g_ready) return;
    waitIdle(5000);
    g_painter->clearBuffers();
    g_painter->end();
    g_ready = false;
}

int textWidth(const char* s, const GFXfont* font) {
    int16_t x1, y1; uint16_t w, h;
    g_canvas.setFont(font);
    g_canvas.getTextBounds(s, 0, 100, &x1, &y1, &w, &h);
    return (int)w;
}

int capHeight(const GFXfont* font) {
    int16_t x1, y1; uint16_t w, h;
    g_canvas.setFont(font);
    g_canvas.getTextBounds("H", 0, 100, &x1, &y1, &w, &h);
    return (int)h;
}

void text(int x, int y, const char* s, const GFXfont* font, uint8_t color) {
    g_canvas.setFont(font);
    g_canvas.setTextColor(color);
    g_canvas.setTextWrap(false);
    g_canvas.setCursor(x, y);
    g_canvas.print(s);
}

void textCentered(int cx, int y, const char* s, const GFXfont* font, uint8_t color) {
    text(cx - textWidth(s, font) / 2, y, s, font, color);
}

void textRight(int rx, int y, const char* s, const GFXfont* font, uint8_t color) {
    text(rx - textWidth(s, font), y, s, font, color);
}

void textFit(int x, int y, const char* s, const GFXfont* font, int maxWidth, uint8_t color) {
    if (textWidth(s, font) <= maxWidth) { text(x, y, s, font, color); return; }
    char buf[96];
    strlcpy(buf, s, sizeof buf);
    size_t n = strlen(buf);
    while (n > 1) {
        buf[n - 1] = 0;
        n--;
        char tmp[100];
        snprintf(tmp, sizeof tmp, "%s...", buf);
        if (textWidth(tmp, font) <= maxWidth) { text(x, y, tmp, font, color); return; }
    }
}

void upper(char* out, size_t cap, const char* in) {
    size_t n = 0;
    for (; in[n] && n + 1 < cap; ++n) {
        const char c = in[n];
        out[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[n] = 0;
}

// --- tracked labels ----------------------------------------------------------
static const int TRACK = 3;

static int glyphAdvance(const GFXfont* font, unsigned char c) {
    if (c < font->first || c > font->last) return 0;
    return font->glyph[c - font->first].xAdvance;
}

int labelWidth(const char* s, const GFXfont* font) {
    int total = -TRACK;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        total += glyphAdvance(font, c) + TRACK;
    }
    return total < 0 ? 0 : total;
}

void label(int x, int y, const char* s, const GFXfont* font, uint8_t color) {
    g_canvas.setFont(font);
    g_canvas.setTextColor(color);
    g_canvas.setTextWrap(false);
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        g_canvas.setCursor(x, y);
        g_canvas.write(c);
        x += glyphAdvance(font, c) + TRACK;
    }
}

void labelCentered(int cx, int y, const char* s, const GFXfont* font, uint8_t color) {
    label(cx - labelWidth(s, font) / 2, y, s, font, color);
}

// --- shapes -----------------------------------------------------------------
void frame(int x, int y, int w, int h, int thick, uint8_t color) {
    for (int e = 0; e < thick; ++e) g_canvas.drawRect(x + e, y + e, w - 2 * e, h - 2 * e, color);
}

void rule(int x, int y, int w, int thick, uint8_t color) {
    g_canvas.fillRect(x, y, w, thick, color);
}

void tone50(int x, int y, int w, int h) {
    if (!g_fb) return;
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t* row = g_fb + (size_t)yy * W;
        for (int xx = x0; xx < x1; ++xx)
            if (((xx >> 1) + (yy >> 1)) & 1) row[xx] = BLACK;
    }
}

void batteryIcon(int rightX, int cy, int pct, bool charging) {
    const int bw = 36, bh = 18, nub = 3;
    const int x = rightX - bw - nub, y = cy - bh / 2;
    frame(x, y, bw, bh, 2, BLACK);
    g_canvas.fillRect(x + bw, cy - 4, nub, 8, BLACK);
    if (charging) {
        // A bolt instead of the level: the percentage next to the icon says how full.
        const int cx = x + bw / 2;
        g_canvas.fillTriangle(cx + 4, y + 3, cx - 6, cy + 2, cx + 1, cy + 2, BLACK);
        g_canvas.fillTriangle(cx - 4, y + bh - 4, cx + 6, cy - 2, cx - 1, cy - 2, BLACK);
    } else if (pct >= 0) {
        const int fw = (bw - 8) * (pct > 100 ? 100 : pct) / 100;
        if (fw > 0) g_canvas.fillRect(x + 4, y + 4, fw, bh - 8, BLACK);
    }
}

}  // namespace display
