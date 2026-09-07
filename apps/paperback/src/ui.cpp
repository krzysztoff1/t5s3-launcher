#include "ui.h"

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "hn.h"
#include "covers.h"
#include "hwio.h"
#include "utf8.h"

using namespace display;

namespace ui {

struct Hit { int16_t x, y, w, h, id, arg; };
static Hit g_hits[48];
static int g_hitCount = 0;

void hitsClear() { g_hitCount = 0; }
void hit(int x, int y, int w, int h, int id, int arg) {
    if (g_hitCount >= (int)(sizeof g_hits / sizeof g_hits[0])) return;
    g_hits[g_hitCount++] = Hit{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)id, (int16_t)arg};
}
int hitTest(int x, int y, int* arg) {
    for (int i = g_hitCount - 1; i >= 0; --i) {
        const Hit& h = g_hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) { if (arg) *arg = h.arg; return h.id; }
    }
    return -1;
}

void batteryIcon(int x, int y, int pct, bool usb) {
    // 26x13 body + nub, y is the top.
    rect(x, y, 24, 13, BLACK, 1);
    fillRect(x + 24, y + 4, 2, 5, BLACK);
    if (pct >= 0) {
        const int w = (20 * pct + 50) / 100;
        if (w > 0) fillRect(x + 2, y + 2, w, 9, pct <= 15 ? DARK : BLACK);
    }
    if (usb) {
        // small bolt in white over the fill
        const int cx = x + 12;
        for (int i = 0; i < 4; ++i) pixel(cx - 1 + i, y + 2 + i, WHITE);
        for (int i = 0; i < 4; ++i) pixel(cx + 2 - i, y + 6 + i, WHITE);
        fillRect(cx - 1, y + 5, 5, 2, WHITE);
    }
}

void status(int rx, int baseline) {
    char buf[16] = {0};
    int h, m;
    if (hwio::clock(h, m)) snprintf(buf, sizeof buf, "%02d:%02d", h, m);
    const int pct = hwio::batteryPercent();
    const bool usb = hwio::usbPowered();
    int x = rx - 26;
    batteryIcon(x, baseline - 12, pct, usb);
    if (pct >= 0) {
        char p[8]; snprintf(p, sizeof p, "%d%%", pct);
        const float w = font::width(p, font::SANS, 15);
        x -= (int)w + 8;
        font::draw(x, baseline, p, font::SANS, 15, DARK);
    }
    if (buf[0]) {
        const float w = font::width(buf, font::SANS_BOLD, 16);
        x -= (int)w + 14;
        font::draw(x, baseline, buf, font::SANS_BOLD, 16, BLACK);
    }
}

void header(const char* title, const char* backLabel, int backId) {
    // 84 px band: back target | title | clock + battery, 1 px rule under it.
    // The back target is the whole left end of the band, at least 200 x 96 —
    // the label is small, the thing you aim at is not.
    int x = MARGIN;
    if (backLabel) {
        chevron(x + 10, HEADER_BASE - 9, 11, false, BLACK, 3);
        x += 32;
        font::draw(x, HEADER_BASE, backLabel, font::SANS, 18, BLACK);
        x += (int)font::width(backLabel, font::SANS, 18) + 26;
        const int hitW = x + 8 > 200 ? x + 8 : 200;
        hit(0, 0, hitW, HEADER_HIT_H, backId, 0);
        vline(x - 13, HEADER_BASE - 24, 32, LIGHT);
    }
    font::drawFit(x, HEADER_BASE, title, font::SANS_BOLD, 26, W - x - 170, BLACK);
    status(W - MARGIN, HEADER_BASE - 2);
    hline(MARGIN, HEADER_H - 1, W - 2 * MARGIN, LIGHT);
}

void footer(const char* text) {
    font::drawCentered(W / 2, H - 22, text, font::SANS, 13, DARK);
}

void sectionLabel(int x, int baseline, const char* text) {
    // Tracked-out small caps effect: draw upper-case with extra spacing.
    char up[64]; size_t n = 0;
    for (const char* p = text; *p && n + 1 < sizeof up; ++p) up[n++] = (char)toupper((unsigned char)*p);
    up[n] = 0;
    float pen = (float)x;
    int prev = -1;
    for (size_t i = 0; i < n; ++i) {
        char c[2] = {up[i], 0};
        pen += font::draw((int)pen, baseline, c, font::SANS_BOLD, 13, DARK) + 1.6f;
        (void)prev;
    }
}

void chevron(int cx, int cy, int size, bool right, uint8_t ink, int thick) {
    for (int i = 0; i < size; ++i) {
        const int dx = right ? i : -i;
        for (int t = 0; t < thick; ++t) {
            pixel(cx + dx - (right ? size / 2 : -size / 2), cy - size + i + t, ink);
            pixel(cx + dx - (right ? size / 2 : -size / 2), cy + size - i - t, ink);
        }
    }
}

void button(int x, int y, int w, int h, const char* label, bool primary, int id, int arg, bool enabled) {
    // Drawn at least 72 px tall, hit at least TOUCH_MIN tall (centred on the
    // drawn shape), so no caller can make a button too small to aim at.
    if (h < 72) { y -= (72 - h) / 2; h = 72; }
    const int r = 12;
    if (primary && enabled) {
        fillRoundRect(x, y, w, h, r, BLACK);
    } else {
        roundRect(x, y, w, h, r, enabled ? BLACK : LIGHT, enabled ? 2 : 1);
    }
    const uint8_t ink = (primary && enabled) ? WHITE : (enabled ? BLACK : LIGHT);
    const float px = h >= 80 ? 20.f : 18.f;
    const int base = y + h / 2 + (int)(px * 0.36f);
    const float tw = font::width(label, font::SANS_BOLD, px);
    if (tw <= w - 28) font::drawCentered(x + w / 2, base, label, font::SANS_BOLD, px, ink);
    else font::drawFit(x + 14, base, label, font::SANS_BOLD, px, w - 28, ink);
    if (enabled) {
        const int hh = h < TOUCH_MIN ? TOUCH_MIN : h;
        hit(x - 4, y - (hh - h) / 2 - 4, w + 8, hh + 8, id, arg);
    }
}

void chevronButton(int x, int y, int w, int h, bool right, int id, int arg, bool enabled) {
    roundRect(x, y, w, h, 10, enabled ? BLACK : LIGHT, enabled ? 2 : 1);
    chevron(x + w / 2, y + h / 2, 12, right, enabled ? BLACK : LIGHT, 3);
    if (enabled) hit(x - 4, y - 4, w + 8, h + 8, id, arg);
}

void iconButton(int x, int y, int w, int h, Icon icon, const char* label, int id, int arg, bool enabled) {
    roundRect(x, y, w, h, 12, enabled ? BLACK : LIGHT, enabled ? 2 : 1);
    const uint8_t ink = enabled ? BLACK : LIGHT;
    const int cx = x + w / 2;
    // Icon in the upper half, label on a baseline 16 px above the bottom edge.
    const int iy = y + h / 2 - 14;
    switch (icon) {
        case ICON_HN:      hnLogo(cx - 14, iy - 14, 28); break;
        case ICON_GEAR:    iconGear(cx, iy, 14, ink); break;
        case ICON_REFRESH: iconRefresh(cx, iy, 12, ink); break;
        case ICON_MOON:    iconMoon(cx, iy, 11, ink); break;
    }
    const float tw = font::width(label, font::SANS_BOLD, 14);
    if (tw <= w - 16) font::drawCentered(cx, y + h - 16, label, font::SANS_BOLD, 14, ink);
    else font::drawFit(x + 8, y + h - 16, label, font::SANS_BOLD, 14, w - 16, ink);
    if (enabled) hit(x - 4, y - 4, w + 8, h + 8, id, arg);
}

void tabs(int x, int y, int w, int h, const char* const* labels, int count, int active, int id) {
    if (count <= 0) return;
    roundRect(x, y, w, h, 12, BLACK, 2);
    const int cw = w / count;
    for (int i = 0; i < count; ++i) {
        const int cx = x + i * cw;
        const bool on = i == active;
        if (on) {
            // Filled cell: square inner corners except at the control's ends.
            fillRoundRect(cx, y, i == count - 1 ? w - i * cw : cw, h, 12, BLACK);
            if (i > 0) fillRect(cx, y, 12, h, BLACK);
            if (i < count - 1) fillRect(cx + cw - 12, y, 12, h, BLACK);
        }
        if (i > 0 && !on) vline(cx, y + 2, h - 4, BLACK);
        const int base = y + h / 2 + 7;
        font::drawCentered(cx + cw / 2, base, labels[i], font::SANS_BOLD, 19, on ? WHITE : BLACK);
        hit(cx, y - 4, cw, h + 8, id, i);
    }
}

void progressBar(int x, int y, int w, int h, float frac, const uint16_t* ticksPermille, int nTicks) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    fillRect(x, y, w, h, LIGHT);
    const int fw = (int)lroundf(frac * w);
    if (fw > 0) fillRect(x, y, fw, h, BLACK);
    for (int i = 0; i < nTicks; ++i) {
        const int tx = x + (int)((long)ticksPermille[i] * w / 1000);
        fillRect(tx, y - 3, 1, h + 6, tx <= x + fw ? BLACK : DARK);
    }
}

void checkBadge(int cx, int cy, int r) {
    fillCircle(cx, cy, r, BLACK);
    // check mark
    for (int i = 0; i < r / 2; ++i) { pixel(cx - r / 2 + i, cy + i, WHITE); pixel(cx - r / 2 + i, cy + i + 1, WHITE); }
    for (int i = 0; i < r; ++i) { pixel(cx - r / 2 + r / 2 + i, cy + r / 2 - i, WHITE); pixel(cx - r / 2 + r / 2 + i, cy + r / 2 - i + 1, WHITE); }
}

void dots(int cx, int y, int count, int active) {
    if (count <= 1) return;
    const int gap = 14;
    int x = cx - (count - 1) * gap / 2;
    for (int i = 0; i < count; ++i, x += gap) {
        if (i == active) fillCircle(x, y, 4, BLACK); else circle(x, y, 3, DARK);
    }
}

void formatDuration(char* out, size_t cap, float seconds) {
    if (seconds < 60) { snprintf(out, cap, "under a minute"); return; }
    const int mins = (int)(seconds / 60 + 0.5f);
    if (mins < 60) { snprintf(out, cap, "%d min", mins); return; }
    const int h = mins / 60, m = mins % 60;
    if (m == 0) snprintf(out, cap, "%d h", h);
    else snprintf(out, cap, "%d h %d min", h, m);
}

int wrapLines(const char* s, font::Face f, float px, int w, int maxLines) {
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
            const char* we = q;
            while (we < e && *we != ' ') ++we;
            if (font::width(lineStart, we, f, px) > w) break;
            lastFit = we;
            q = we;
            while (q < e && *q == ' ') ++q;
        }
        if (!lastFit) { lastFit = lineStart; while (lastFit < e && *lastFit != ' ') ++lastFit; }
        lines++;
        p = lastFit;
    }
    if (p < e) lines++;    // did not fit: signal overflow
    return lines;
}

// ---- covers -------------------------------------------------------------------------

static void coverTitle(int x, int y, int w, int h, const char* title, font::Face face, uint8_t ink, int align) {
    // Find the largest size whose wrapped title fits in h.
    float px = h * 0.42f;
    if (px > 40) px = 40;
    const float minPx = 11.f;
    int lines = 1;
    for (; px >= minPx; px -= 1.f) {
        const int lh = (int)(px * 1.15f);
        const int maxLines = h / lh;
        if (maxLines < 1) continue;
        lines = wrapLines(title, face, px, w, maxLines);
        if (lines <= maxLines) break;
    }
    if (px < minPx) px = minPx;
    const int lh = (int)(px * 1.15f);
    const int maxLines = h / lh < 1 ? 1 : h / lh;
    if (lines > maxLines) lines = maxLines;
    const int y0 = y + (h - lines * lh) / 2 + (int)(px * 0.85f);
    font::drawWrapped(x, y0, w, title, face, px, lh, maxLines, ink, align);
}

void cover(int x, int y, int w, int h, const library::Book& b, bool selected) {
    if (covers::draw(x, y, w, h, b)) {
        if (b.finished) checkBadge(x + w - 14, y + 14, 10);
        if (selected) roundRect(x - 5, y - 5, w + 10, h + 10, 6, BLACK, 3);
        return;
    }
    const int variant = (int)(b.id % 5);
    const float authorPx = h * 0.065f > 11 ? h * 0.065f : 11;
    const int pad = w / 12;
    fillRect(x, y, w, h, WHITE);
    switch (variant) {
    case 0: {   // classic double frame
        rect(x, y, w, h, BLACK, 2);
        rect(x + pad / 2 + 2, y + pad / 2 + 2, w - pad - 4, h - pad - 4, BLACK, 1);
        const int th = (int)(h * 0.42f);
        coverTitle(x + pad + 2, y + (int)(h * 0.16f), w - 2 * pad - 4, th, b.title, font::SERIF_BOLD, BLACK, 1);
        hline(x + w / 2 - w / 6, y + (int)(h * 0.64f), w / 3, BLACK);
        fillCircle(x + w / 2, y + (int)(h * 0.64f), 2, BLACK);
        font::drawWrapped(x + pad + 2, y + (int)(h * 0.64f) + (int)(authorPx * 1.6f), w - 2 * pad - 4, b.author, font::SERIF_ITALIC, authorPx, (int)(authorPx * 1.2f), 2, BLACK, 1);
        break;
    }
    case 1: {   // black band
        const int band = (int)(h * 0.56f);
        fillRect(x, y, w, band, BLACK);
        coverTitle(x + pad, y + pad, w - 2 * pad, band - 2 * pad, b.title, font::SERIF_BOLD, WHITE, 0);
        rect(x, y, w, h, BLACK, 2);
        font::drawWrapped(x + pad, y + band + (int)(authorPx * 1.5f), w - 2 * pad, b.author, font::SANS, authorPx, (int)(authorPx * 1.2f), 2, BLACK, 0);
        hline(x + pad, y + h - pad - 2, w / 4, BLACK);
        break;
    }
    case 2: {   // ruled paper with a label
        pattern(x, y, w, h, 2, LIGHT);
        rect(x, y, w, h, BLACK, 2);
        const int lw = w - 2 * pad, lh = (int)(h * 0.44f);
        const int lx = x + pad, ly = y + (int)(h * 0.22f);
        fillRect(lx, ly, lw, lh, WHITE);
        rect(lx, ly, lw, lh, BLACK, 2);
        coverTitle(lx + 6, ly + 6, lw - 12, lh - 12, b.title, font::SERIF_BOLD, BLACK, 1);
        fillRect(x + 2, y + h - (int)(authorPx * 2.4f), w - 4, (int)(authorPx * 2.4f) - 2, WHITE);
        font::drawWrapped(x + pad, y + h - (int)(authorPx * 1.0f), w - 2 * pad, b.author, font::SANS, authorPx, (int)(authorPx * 1.2f), 1, BLACK, 1);
        break;
    }
    case 3: {   // hatched top, dark bottom band
        pattern(x, y, w, h, 1, LIGHT);
        const int band = (int)(h * 0.42f);
        fillRect(x, y + h - band, w, band, BLACK);
        rect(x, y, w, h, BLACK, 2);
        coverTitle(x + pad, y + h - band + pad / 2, w - 2 * pad, band - pad, b.title, font::SERIF_BOLD, WHITE, 0);
        fillRect(x + 2, y + h - band - (int)(authorPx * 1.9f), w - 4, (int)(authorPx * 1.9f), WHITE);
        font::drawWrapped(x + pad, y + h - band - (int)(authorPx * 0.6f), w - 2 * pad, b.author, font::SANS, authorPx, (int)(authorPx * 1.2f), 1, BLACK, 0);
        break;
    }
    default: {  // big initial
        rect(x, y, w, h, BLACK, 2);
        fillRect(x, y, w / 14 + 2, h, BLACK);
        char init[8] = {0};
        const char* t = b.title;
        const char* te = t + strlen(t);
        const char* q = t;
        utf8Next(q, te);
        memcpy(init, t, (size_t)(q - t));
        font::draw(x + w / 14 + pad, y + (int)(h * 0.46f), init, font::SERIF_BOLD, h * 0.5f, DARK);
        coverTitle(x + w / 14 + pad, y + (int)(h * 0.5f), w - w / 14 - 2 * pad, (int)(h * 0.3f), b.title, font::SERIF_BOLD, BLACK, 0);
        font::drawWrapped(x + w / 14 + pad, y + h - (int)(authorPx * 1.0f), w - w / 14 - 2 * pad, b.author, font::SANS, authorPx, (int)(authorPx * 1.2f), 1, BLACK, 0);
        break;
    }
    }
    if (b.finished) checkBadge(x + w - 14, y + 14, 10);
    if (selected) roundRect(x - 5, y - 5, w + 10, h + 10, 6, BLACK, 3);
}

void hnLogo(int x, int y, int size) {
    fillRoundRect(x, y, size, size, size / 8, BLACK);
    // Y in white
    const int cx = x + size / 2, top = y + size / 5, mid = y + size / 2, bot = y + size - size / 5;
    const int arm = size / 4;
    for (int i = 0; i <= mid - top; ++i) {
        const int t = i * arm / (mid - top);
        for (int k = 0; k < size / 10; ++k) { pixel(cx - arm + t + k, top + i, WHITE); pixel(cx + arm - t - k, top + i, WHITE); }
    }
    fillRect(cx - size / 20, mid, size / 10, bot - mid, WHITE);
}

void iconMoon(int cx, int cy, int r, uint8_t ink) {
    fillCircle(cx, cy, r, ink);
    fillCircle(cx + r / 2, cy - r / 3, r - 1, WHITE);
}
void iconSun(int cx, int cy, int r, uint8_t ink) {
    circle(cx, cy, r / 2, ink);
    for (int a = 0; a < 8; ++a) {
        const float t = a * 0.785398f;
        const int x0 = cx + (int)(cosf(t) * (r / 2 + 3)), y0 = cy + (int)(sinf(t) * (r / 2 + 3));
        const int x1 = cx + (int)(cosf(t) * r), y1 = cy + (int)(sinf(t) * r);
        for (int i = 0; i <= 4; ++i) pixel(x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4, ink);
    }
}
void iconGear(int cx, int cy, int r, uint8_t ink) {
    circle(cx, cy, r - 3, ink); circle(cx, cy, r - 4, ink);
    for (int a = 0; a < 8; ++a) {
        const float t = a * 0.785398f;
        fillRect(cx + (int)(cosf(t) * (r - 2)) - 1, cy + (int)(sinf(t) * (r - 2)) - 1, 3, 3, ink);
    }
    fillCircle(cx, cy, 2, ink);
}
void iconList(int cx, int cy, int r, uint8_t ink) {
    for (int i = -1; i <= 1; ++i) {
        fillRect(cx - r + 4, cy + i * (r * 2 / 3) - 1, 2 * r - 4, 2, ink);
        fillRect(cx - r, cy + i * (r * 2 / 3) - 1, 2, 2, ink);
    }
}
void iconRefresh(int cx, int cy, int r, uint8_t ink) {
    for (int a = 30; a < 330; a += 6) {
        const float t = a * 0.0174533f;
        pixel(cx + (int)(cosf(t) * r), cy + (int)(sinf(t) * r), ink);
        pixel(cx + (int)(cosf(t) * (r - 1)), cy + (int)(sinf(t) * (r - 1)), ink);
    }
    // arrow head at the top right
    const int ax = cx + (int)(cosf(0.5236f) * r), ay = cy + (int)(sinf(0.5236f) * r);
    for (int i = 0; i < 4; ++i) { fillRect(ax - i, ay - 4 + i, 1 + i, 1, ink); }
}

void toast(const char* line1, const char* line2) {
    const int w = 380, h = line2 ? 118 : 90;
    const int x = (W - w) / 2, y = (H - h) / 2;
    fillRoundRect(x - 3, y - 3, w + 6, h + 6, 16, WHITE);
    roundRect(x, y, w, h, 14, BLACK, 2);
    font::drawFit(x + 24, y + (line2 ? 46 : 54), line1, font::SANS_BOLD, 20, w - 48, BLACK);
    if (line2) font::drawWrapped(x + 24, y + 78, w - 48, line2, font::SANS, 15, 19, 2, DARK, 0);
    paint();
}

}  // namespace ui
