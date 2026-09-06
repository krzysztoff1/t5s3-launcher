#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "display.h"
#include "hw.h"
#include "registry.h"
#include "syscheck.h"
#include "version.h"

using namespace display;

namespace ui {

enum Action { ACT_NONE, ACT_BOOT, ACT_SYSCHECK, ACT_AUTOSTART, ACT_SLEEP, ACT_FLASH };

struct Item {
    Action action;
    int    slot;
    char   title[40];
    char   sub[72];
    bool   enabled;
    int    y0, y1;
};

static Item  g_items[10];
static int   g_count = 0;
static int   g_sel = 0;
static char  g_notice[96] = {0};
static uint32_t g_lastInput = 0;
static bool  g_ready = false;

static const int ROW_H = 104;
static const int ROWS_Y = 150;
static const uint32_t IDLE_SLEEP_MS = 5UL * 60UL * 1000UL;

void setNotice(const char* msg) {
    strlcpy(g_notice, msg ? msg : "", sizeof g_notice);
}

static void buildItems() {
    g_count = 0;
    registry::Slot slots[8];
    const int n = registry::scan(slots, 8);
    for (int i = 0; i < n && g_count < 6; ++i) {
        const registry::Slot& s = slots[i];
        if (s.index < 0) continue;                 // the launcher itself
        Item& it = g_items[g_count++];
        it.action = ACT_BOOT;
        it.slot = s.index;
        it.enabled = s.valid;
        if (s.valid) {
            strlcpy(it.title, s.name, sizeof it.title);
            snprintf(it.sub, sizeof it.sub, "%s%s  -  slot %s%s",
                     s.version, s.version[0] ? "" : "no version", registry::slotLabel(s.index),
                     s.last ? "  -  last run" : "");
        } else {
            snprintf(it.title, sizeof it.title, "Empty slot %s", registry::slotLabel(s.index));
            strlcpy(it.sub, "tools/flash.py app ... to install", sizeof it.sub);
        }
    }
    Item* it = &g_items[g_count++];
    *it = Item{ACT_SYSCHECK, 0, "System check", "Every chip on the board, results on screen + serial", true, 0, 0};
    it = &g_items[g_count++];
    snprintf(it->title, sizeof it->title, "Autostart: %s", registry::autostart() ? "on" : "off");
    strlcpy(it->sub, "Cold boot starts the last app (hold side button for menu)", sizeof it->sub);
    it->action = ACT_AUTOSTART; it->slot = 0; it->enabled = true;
    it = &g_items[g_count++];
    *it = Item{ACT_SLEEP, 0, "Sleep", "Deep sleep now; BOOT wakes back into this menu", true, 0, 0};
    it = &g_items[g_count++];
    *it = Item{ACT_FLASH, 0, "Flash mode", "Reboot into the USB download mode", true, 0, 0};
    if (g_sel >= g_count) g_sel = 0;
}

static void drawHeader() {
    GFXcanvas8& g = gfx();
    g.fillRect(0, 0, W, 96, BLACK);
    text(24, 62, LAUNCHER_NAME, FONT_TITLE, WHITE);
    char right[32];
    const int pct = hw::batteryPercent();
    if (pct >= 0) snprintf(right, sizeof right, "%d%%%s", pct, hw::usbPowered() ? " USB" : "");
    else          snprintf(right, sizeof right, "%s", hw::usbPowered() ? "USB" : "");
    textRight(W - 24, 62, right, FONT_ITEM, WHITE);
    text(24, 128, g_notice[0] ? g_notice : "Tap an app to start it.", FONT_BODY, BLACK);
}

void draw() {
    if (!g_ready) return;
    buildItems();
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    drawHeader();
    int y = ROWS_Y;
    for (int i = 0; i < g_count; ++i) {
        Item& it = g_items[i];
        it.y0 = y; it.y1 = y + ROW_H;
        const bool sel = (i == g_sel);
        if (sel) g.fillRect(12, y, W - 24, ROW_H - 8, LIGHT);
        g.drawRect(12, y, W - 24, ROW_H - 8, it.enabled ? BLACK : LIGHT);
        if (sel) g.fillRect(12, y, 10, ROW_H - 8, BLACK);
        const uint8_t col = it.enabled ? BLACK : DARK;
        textFit(36, y + 42, it.title, FONT_ITEM, W - 72, col);
        textFit(36, y + 76, it.sub, FONT_SMALL, W - 72, col);
        y += ROW_H;
    }
    char foot[96];
    snprintf(foot, sizeof foot, "v%s   BOOT: next / hold: select   side button: next", LAUNCHER_VERSION);
    text(24, H - 28, foot, FONT_SMALL, DARK);
    paint();
}

void banner(const char* title, const char* line) {
    if (!display::begin()) return;
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    textCentered(W / 2, H / 2 - 20, title, FONT_TITLE, BLACK);
    if (line) textCentered(W / 2, H / 2 + 40, line, FONT_BODY, BLACK);
    paint();
}

bool begin() {
    if (!display::begin()) return false;
    g_ready = true;
    hw::touchBegin();
    g_lastInput = millis();
    draw();
    Serial.println("[launcher] menu");
    return true;
}

static void activate(int idx) {
    if (idx < 0 || idx >= g_count) return;
    Item& it = g_items[idx];
    if (!it.enabled) return;
    switch (it.action) {
    case ACT_BOOT: {
        char line[64];
        snprintf(line, sizeof line, "%s", it.title);
        banner("Starting", line);
        registry::boot(it.slot, esp_reset_reason());
        setNotice("Could not start that slot.");
        draw();
        break;
    }
    case ACT_SYSCHECK:
        syscheck::run(true);
        draw();
        break;
    case ACT_AUTOSTART:
        registry::setAutostart(!registry::autostart());
        draw();
        break;
    case ACT_SLEEP:
        banner("Sleeping", "Press BOOT to wake");
        hw::deepSleep();
        break;
    case ACT_FLASH:
        banner("Download mode", "Waiting for tools/flash.py");
        hw::rebootToDownloadMode();
        break;
    default:
        break;
    }
}

void poll() {
    if (!g_ready) return;
    const uint32_t now = millis();

    int tx, ty;
    if (hw::touchRead(tx, ty)) {
        g_lastInput = now;
        for (int i = 0; i < g_count; ++i) {
            if (ty >= g_items[i].y0 && ty < g_items[i].y1) {
                g_sel = i;
                activate(i);
                break;
            }
        }
        // Debounce: wait for release.
        uint32_t t0 = millis();
        while (hw::touchRead(tx, ty) && millis() - t0 < 1500) delay(10);
        return;
    }

    static bool bootWas = false, sideWas = false;
    static uint32_t bootDownAt = 0;
    const bool boot = hw::bootButtonPressed();
    const bool side = hw::sideButtonPressed();
    if (boot && !bootWas) bootDownAt = now;
    if (!boot && bootWas) {
        g_lastInput = now;
        if (now - bootDownAt > 700) activate(g_sel);
        else { g_sel = (g_sel + 1) % g_count; draw(); }
    }
    if (side && !sideWas) {
        g_lastInput = now;
        g_sel = (g_sel + 1) % g_count;
        draw();
    }
    bootWas = boot; sideWas = side;

    if (now - g_lastInput > IDLE_SLEEP_MS && !hw::usbPowered()) {
        banner("Sleeping", "Press BOOT to wake");
        hw::deepSleep();
    }
}

}  // namespace ui
