#include "covers.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>

#include <PNGdec.h>
#include <JPEGDEC.h>

#include "display.h"
#include "library.h"

namespace covers {

struct Entry { uint32_t id; uint8_t* gray; int w, h; uint32_t lastUse; };
static Entry g_cache[12];
static size_t g_cacheBytes = 0;
static const size_t CACHE_MAX = 2 * 1024 * 1024;
static uint32_t g_clock = 0;

static PNG*     g_png = nullptr;
static JPEGDEC* g_jpg = nullptr;

struct DecodeCtx {
    uint8_t* gray;
    int w, h;
    uint16_t* line565;
    bool failed;
};

// ---- PNG ---------------------------------------------------------------------------------

static void pngLine(PNGDRAW* d) {
    DecodeCtx* c = (DecodeCtx*)d->pUser;
    if (!c || d->y < 0 || d->y >= c->h) return;
    uint8_t* row = c->gray + (size_t)d->y * c->w;
    const int n = d->iWidth < c->w ? d->iWidth : c->w;
    if (d->iPixelType == PNG_PIXEL_GRAYSCALE && d->iBpp == 8) {
        memcpy(row, d->pPixels, n);
    } else if (d->iPixelType == PNG_PIXEL_GRAYSCALE && d->iBpp < 8) {
        const int per = 8 / d->iBpp;
        const int mask = (1 << d->iBpp) - 1;
        for (int x = 0; x < n; ++x) {
            const int v = (d->pPixels[x / per] >> (8 - d->iBpp * (1 + x % per))) & mask;
            row[x] = (uint8_t)(v * 255 / mask);
        }
    } else if (d->iPixelType == PNG_PIXEL_GRAYSCALE && d->iBpp == 16) {
        for (int x = 0; x < n; ++x) row[x] = d->pPixels[x * 2];
    } else if (d->iPixelType == PNG_PIXEL_GRAY_ALPHA && d->iBpp == 8) {
        for (int x = 0; x < n; ++x) {
            const int a = d->pPixels[x * 2 + 1];
            row[x] = (uint8_t)((d->pPixels[x * 2] * a + 255 * (255 - a)) / 255);   // over white
        }
    } else if (c->line565) {
        g_png->getLineAsRGB565(d, c->line565, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
        for (int x = 0; x < n; ++x) {
            const uint16_t p = c->line565[x];
            const int r = ((p >> 11) & 0x1F) * 255 / 31, g = ((p >> 5) & 0x3F) * 255 / 63, b = (p & 0x1F) * 255 / 31;
            row[x] = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8);
        }
    } else {
        c->failed = true;
    }
}

static uint8_t* decodePng(const uint8_t* data, uint32_t size, int& w, int& h) {
    if (!g_png) {
        void* mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
        if (!mem) return nullptr;
        g_png = new (mem) PNG();
    }
    if (g_png->openRAM((uint8_t*)data, (int)size, pngLine) != PNG_SUCCESS) {
        Serial.printf("[covers] PNG open failed (%d)\n", g_png->getLastError());
        return nullptr;
    }
    w = g_png->getWidth(); h = g_png->getHeight();
    if (w <= 0 || h <= 0 || w > 1600 || h > 2400) { g_png->close(); return nullptr; }
    DecodeCtx c{(uint8_t*)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM), w, h, nullptr, false};
    if (!c.gray) { g_png->close(); return nullptr; }
    const int type = g_png->getPixelType();
    if (!(type == PNG_PIXEL_GRAYSCALE || type == PNG_PIXEL_GRAY_ALPHA))
        c.line565 = (uint16_t*)heap_caps_malloc((size_t)w * 2, MALLOC_CAP_SPIRAM);
    const int rc = g_png->decode(&c, 0);
    g_png->close();
    if (c.line565) heap_caps_free(c.line565);
    if (rc != PNG_SUCCESS || c.failed) {
        Serial.printf("[covers] PNG decode failed (%d)\n", rc);
        heap_caps_free(c.gray);
        return nullptr;
    }
    return c.gray;
}

// ---- JPEG --------------------------------------------------------------------------------------

static int jpgBlock(JPEGDRAW* d) {
    DecodeCtx* c = (DecodeCtx*)d->pUser;
    if (!c) return 1;
    const uint8_t* px = (const uint8_t*)d->pPixels;
    for (int yy = 0; yy < d->iHeight; ++yy) {
        const int y = d->y + yy;
        if (y < 0 || y >= c->h) continue;
        int n = d->iWidth;
        if (d->x + n > c->w) n = c->w - d->x;
        if (n <= 0) continue;
        memcpy(c->gray + (size_t)y * c->w + d->x, px + (size_t)yy * d->iWidth, (size_t)n);
    }
    return 1;
}

static uint8_t* decodeJpeg(uint8_t* data, uint32_t size, int& w, int& h) {
    if (!g_jpg) {
        void* mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
        if (!mem) return nullptr;
        g_jpg = new (mem) JPEGDEC();
    }
    if (!g_jpg->openRAM(data, (int)size, jpgBlock)) {
        Serial.printf("[covers] JPEG open failed (%d)\n", g_jpg->getLastError());
        return nullptr;
    }
    g_jpg->setPixelType(EIGHT_BIT_GRAYSCALE);
    int scale = 1, opt = 0;
    while (g_jpg->getWidth() / scale > 720 && scale < 8) scale *= 2;
    if (scale == 2) opt = JPEG_SCALE_HALF; else if (scale == 4) opt = JPEG_SCALE_QUARTER; else if (scale == 8) opt = JPEG_SCALE_EIGHTH;
    w = g_jpg->getWidth() / scale; h = g_jpg->getHeight() / scale;
    if (w <= 0 || h <= 0) { g_jpg->close(); return nullptr; }
    DecodeCtx c{(uint8_t*)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM), w, h, nullptr, false};
    if (!c.gray) { g_jpg->close(); return nullptr; }
    memset(c.gray, 255, (size_t)w * h);
    g_jpg->setUserPointer(&c);
    const int ok = g_jpg->decode(0, 0, opt);
    g_jpg->close();
    if (!ok) {
        Serial.printf("[covers] JPEG decode failed (%d)\n", g_jpg->getLastError());
        heap_caps_free(c.gray);
        return nullptr;
    }
    return c.gray;
}

// ---- cache ----------------------------------------------------------------------------------------

static void evictOne() {
    int victim = -1;
    for (int i = 0; i < (int)(sizeof g_cache / sizeof g_cache[0]); ++i) {
        if (g_cache[i].gray && (victim < 0 || g_cache[i].lastUse < g_cache[victim].lastUse)) victim = i;
    }
    if (victim < 0) return;
    g_cacheBytes -= (size_t)g_cache[victim].w * g_cache[victim].h;
    heap_caps_free(g_cache[victim].gray);
    g_cache[victim] = Entry{};
}

void flush() {
    for (auto& e : g_cache) { if (e.gray) heap_caps_free(e.gray); e = Entry{}; }
    g_cacheBytes = 0;
}

bool has(const library::Book& b) { return (b.cover && b.coverLen) || b.coverPath[0]; }

static uint8_t* loadFile(const char* path, uint32_t& size) {
    File f = SD.open(path, FILE_READ);
    if (!f) return nullptr;
    size = (uint32_t)f.size();
    if (size == 0 || size > 3u * 1024 * 1024) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return nullptr; }
    uint32_t got = 0;
    while (got < size) {
        const int n = f.read(buf + got, size - got > 32768 ? 32768 : size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    f.close();
    if (got != size) { heap_caps_free(buf); return nullptr; }
    return buf;
}

static const Entry* get(const library::Book& b) {
    for (auto& e : g_cache) if (e.gray && e.id == b.id) { e.lastUse = ++g_clock; return &e; }
    if (!has(b)) return nullptr;
    int w = 0, h = 0;
    uint8_t* gray = nullptr;
    const uint32_t t0 = millis();
    if (b.cover && b.coverLen) {
        gray = decodePng(b.cover, b.coverLen, w, h);
    } else if (b.coverPath[0] && library::sdMounted()) {
        uint32_t size = 0;
        uint8_t* data = loadFile(b.coverPath, size);
        if (data) {
            const size_t l = strlen(b.coverPath);
            const bool png = l > 4 && strcasecmp(b.coverPath + l - 4, ".png") == 0;
            gray = png ? decodePng(data, size, w, h) : decodeJpeg(data, size, w, h);
            heap_caps_free(data);
        }
    }
    if (!gray) return nullptr;
    Serial.printf("[covers] %s: %dx%d in %lu ms\n", b.title, w, h, (unsigned long)(millis() - t0));
    while (g_cacheBytes + (size_t)w * h > CACHE_MAX && g_cacheBytes) evictOne();
    Entry* slot = nullptr;
    for (auto& e : g_cache) if (!e.gray) { slot = &e; break; }
    if (!slot) { evictOne(); for (auto& e : g_cache) if (!e.gray) { slot = &e; break; } }
    if (!slot) { heap_caps_free(gray); return nullptr; }
    *slot = Entry{b.id, gray, w, h, ++g_clock};
    g_cacheBytes += (size_t)w * h;
    return slot;
}

bool draw(int x, int y, int w, int h, const library::Book& b) {
    const Entry* e = get(b);
    if (!e) return false;
    const float s = ((float)w / e->w < (float)h / e->h) ? (float)w / e->w : (float)h / e->h;
    int dw = (int)(e->w * s), dh = (int)(e->h * s);
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    const int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
    display::fillRect(x, y, w, h, display::WHITE);
    display::drawGray8(dx, dy, dw, dh, e->gray, e->w, e->h);
    display::rect(x, y, w, h, display::DARK, 1);
    return true;
}

}  // namespace covers
