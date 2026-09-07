#include "display.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"
#include "hwio.h"

namespace display {

static uint8_t*     g_fb = nullptr;
static EPD_Painter* g_painter = nullptr;
static bool         g_ready = false;
static bool         g_fast = false;

bool ready() { return g_ready; }
uint8_t* fb() { return g_fb; }

bool begin(bool scrub) {
    if (g_ready) return true;
    if (!g_fb) {
        g_fb = (uint8_t*)heap_caps_aligned_alloc(16, (size_t)W * H, MALLOC_CAP_SPIRAM);
        if (!g_fb) {
            Serial.println("[display] PSRAM framebuffer alloc failed");
            return false;
        }
    }
    memset(g_fb, WHITE, (size_t)W * H);

    // The init recipe from launcher/src/display.cpp (which took it from
    // OpenTrailPaper's epd_compat.cpp):
    //  * hand the driver OUR Wire, or its own second TwoWire(0) comes up with no
    //    buffers and powerctl init fails fatally;
    //  * auto-shutdown must be off, or begin() may never return;
    //  * push the driver's 129,600-byte fast buffer to PSRAM by making a big
    //    internal block unavailable during begin(), so internal RAM stays free.
    EPD_Painter::Config cfg = EPD_LILYGO_T5_S3_GPS_PRESET;
    cfg.i2c.wire = &Wire;
    static EPD_Painter painter(cfg, /*portrait=*/true);
    painter.setAutoShutdown(false);

    const size_t fastBytes = (size_t)960 * 540 / 4;
    const size_t keepFree  = 48 * 1024;
    void* ballast = nullptr;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest > fastBytes + keepFree) ballast = heap_caps_malloc(largest - keepFree, MALLOC_CAP_INTERNAL);
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
    hwio::rearmSideButton();   // the driver just took the button's expander pin
    painter.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
    Serial.printf("[display] ready, %d grey levels, internal free %u\n",
                  painter.greyLevels(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (scrub) hardClear(2);
    return true;
}

static void waitIdle(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (g_painter && !g_painter->paintIdle() && millis() - t0 < timeoutMs) delay(5);
}

void setFast(bool fast) {
    if (!g_ready || fast == g_fast) return;
    g_fast = fast;
    g_painter->setQuality(fast ? EPD_Painter::Quality::QUALITY_FAST : EPD_Painter::Quality::QUALITY_NORMAL);
}

void paint() {
    if (!g_ready) return;
    g_painter->paint(g_fb);
    waitIdle(6000);
}

void clearRegion(int x, int y, int w, int h, bool hard) {
    if (!g_ready) return;
    EPD_Painter::Rect r{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    g_painter->clear(&r, 1, hard ? EPD_Painter::ClearMode::HARD : EPD_Painter::ClearMode::SOFT);
    waitIdle(6000);
}

void hardClear(int passes) {
    if (!g_ready) return;
    for (int i = 0; i < passes; ++i) {
        g_painter->clear();
        waitIdle(6000);
        delay(60);
    }
    memset(g_fb, WHITE, (size_t)W * H);
}

void scrub() {
    if (!g_ready) return;
    g_painter->clear();
    waitIdle(6000);
}

void end() {
    if (!g_ready) return;
    waitIdle(6000);
    g_painter->clearBuffers();
    g_painter->end();
    g_ready = false;
}

// ---- primitives -------------------------------------------------------------

void fill(uint8_t c) { memset(g_fb, c, (size_t)W * H); }

void fillRect(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint8_t* row = g_fb + (size_t)y * W + x;
    for (int i = 0; i < h; ++i, row += W) memset(row, c, w);
}

void hline(int x, int y, int w, uint8_t c) { fillRect(x, y, w, 1, c); }
void vline(int x, int y, int h, uint8_t c) { fillRect(x, y, 1, h, c); }

void rect(int x, int y, int w, int h, uint8_t c, int thick) {
    fillRect(x, y, w, thick, c);
    fillRect(x, y + h - thick, w, thick, c);
    fillRect(x, y, thick, h, c);
    fillRect(x + w - thick, y, thick, h, c);
}

void fillCircle(int cx, int cy, int r, uint8_t c) {
    for (int dy = -r; dy <= r; ++dy) {
        const int dx = (int)sqrtf((float)(r * r - dy * dy));
        fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

void circle(int cx, int cy, int r, uint8_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pixel(cx + x, cy + y, c); pixel(cx - x, cy + y, c); pixel(cx + x, cy - y, c); pixel(cx - x, cy - y, c);
        pixel(cx + y, cy + x, c); pixel(cx - y, cy + x, c); pixel(cx + y, cy - x, c); pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void fillRoundRect(int x, int y, int w, int h, int r, uint8_t c) {
    if (r <= 0) { fillRect(x, y, w, h, c); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fillRect(x + r, y, w - 2 * r, h, c);
    for (int dy = 0; dy < r; ++dy) {
        const int dx = (int)sqrtf((float)(r * r - (r - dy - 1) * (r - dy - 1)) + 0.5f);
        fillRect(x + r - dx, y + dy, dx, 1, c);
        fillRect(x + w - r, y + dy, dx, 1, c);
        fillRect(x + r - dx, y + h - 1 - dy, dx, 1, c);
        fillRect(x + w - r, y + h - 1 - dy, dx, 1, c);
    }
    fillRect(x, y + r, r, h - 2 * r, c);
    fillRect(x + w - r, y + r, r, h - 2 * r, c);
}

void roundRect(int x, int y, int w, int h, int r, uint8_t c, int thick) {
    // Outer shape minus inner shape, drawn as a filled ring of the same colour
    // over a saved interior would need a scratch buffer; do it by rows instead.
    if (r <= 0) { rect(x, y, w, h, c, thick); return; }
    for (int t = 0; t < thick; ++t) {
        const int rr = r - t;
        const int xx = x + t, yy = y + t, ww = w - 2 * t, hh = h - 2 * t;
        if (ww <= 0 || hh <= 0) break;
        hline(xx + rr, yy, ww - 2 * rr, c);
        hline(xx + rr, yy + hh - 1, ww - 2 * rr, c);
        vline(xx, yy + rr, hh - 2 * rr, c);
        vline(xx + ww - 1, yy + rr, hh - 2 * rr, c);
        // corner arcs
        int cx0 = xx + rr, cy0 = yy + rr, cx1 = xx + ww - 1 - rr, cy1 = yy + hh - 1 - rr;
        int px = rr, py = 0, err = 1 - rr;
        while (px >= py) {
            pixel(cx1 + px, cy1 + py, c); pixel(cx1 + py, cy1 + px, c);
            pixel(cx0 - px, cy1 + py, c); pixel(cx0 - py, cy1 + px, c);
            pixel(cx1 + px, cy0 - py, c); pixel(cx1 + py, cy0 - px, c);
            pixel(cx0 - px, cy0 - py, c); pixel(cx0 - py, cy0 - px, c);
            py++;
            if (err < 0) err += 2 * py + 1;
            else { px--; err += 2 * (py - px) + 1; }
        }
    }
}

void pattern(int x, int y, int w, int h, int kind, uint8_t c) {
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= H) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= W) continue;
            bool on = false;
            switch (kind) {
                case 0: on = ((xx & 7) == 0) && ((yy & 7) == 0); break;             // sparse dots
                case 1: on = (((xx + yy) & 15) < 2); break;                             // diagonal hatch
                case 2: on = ((yy & 7) == 0); break;                                    // horizontal rules
                default: on = (((xx >> 4) + (yy >> 4)) & 1) && ((xx ^ yy) & 1); break;  // checker mesh
            }
            if (on) g_fb[yy * W + xx] = c;
        }
    }
}

void drawGray8(int x, int y, int w, int h, const uint8_t* src, int srcW, int srcH) {
    if (!g_ready || !src || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > W || y + h > H) return;
    g_painter->drawGray8(g_fb, W, H, src, srcW, srcH, x, y, w, h);
}

void tint(int x, int y, int w, int h, uint8_t c) {
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= H) continue;
        uint8_t* row = g_fb + (size_t)yy * W;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= W) continue;
            if (row[xx] == WHITE) row[xx] = c;
        }
    }
}

}  // namespace display
