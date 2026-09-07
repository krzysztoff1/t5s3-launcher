#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_idf_version.h>
#include <esp_system.h>

#include "display.h"
#include "hw.h"
#include "launcher_api.h"
#include "registry.h"
#include "syscheck.h"
#include "version.h"

using namespace display;

namespace ui {

// ---------------------------------------------------------------------------
// Design system, borrowed from OpenTrailPaper's "Device UI System" (see its
// src/ui_render.h): a 12 px step, a 24 px margin, three bands (status / body /
// footer), pure black on paper, rows as full-width bands closed by a 2 px rule,
// state shown by inverting a whole row, and no touch target under 88 px.
// ---------------------------------------------------------------------------
constexpr int MARGIN      = 24;
constexpr int CONTENT_X   = MARGIN;
constexpr int CONTENT_W   = W - 2 * MARGIN;         // 492
constexpr int CELL_PAD    = 16;
constexpr int TEXT_X      = CONTENT_X + CELL_PAD;   // 40
constexpr int GUTTER      = 12;
constexpr int STATUS_H    = 64;                     // status band incl. its 3 px rule
constexpr int ROW_H       = 148;                    // list row
constexpr int DENSE_ROW_H = 108;                    // settings row
constexpr int BTN_H       = 96;                     // strip / sheet buttons
constexpr int RULE        = 2;
constexpr int RULE_HEAVY  = 3;
constexpr int MARKER_W    = 20;                     // row affordance: solid triangle, 20 x 28
constexpr int FOOTER_BASE = H - MARGIN;             // 936

// Settings controls. SWITCH: drawn 120 x 52, the hit rect is the whole row.
constexpr int SWITCH_W = 120, SWITCH_H = 52;
constexpr int SWITCH_X = CONTENT_X + CONTENT_W - CELL_PAD - SWITCH_W;                     // 380
// STEPPER: [88 target] 12 [80 value] 12 [88 target], right-aligned on the padding.
constexpr int STEP_BTN = 88, STEP_GAP = 12, STEP_VALUE_W = 80;
constexpr int STEP_PLUS_X   = CONTENT_X + CONTENT_W - CELL_PAD - STEP_BTN;                 // 412
constexpr int STEP_MINUS_X  = STEP_PLUS_X - STEP_GAP - STEP_VALUE_W - STEP_GAP - STEP_BTN; // 220
constexpr int STEP_VALUE_CX = STEP_MINUS_X + STEP_BTN + STEP_GAP + STEP_VALUE_W / 2;       // 360

// Screen furniture.
constexpr int NOTICE_H       = 96;
constexpr int POWER_BASE     = 760;                             // battery line on SETTINGS
constexpr int BUILD_BASE     = 792;                             // build line on SETTINGS
constexpr int BACK_Y         = FOOTER_BASE - BTN_H;             // 840
constexpr int SHEET_Y        = 440;
constexpr int SHEET_BTN1_Y   = 700, SHEET_BTN2_Y = 816;
constexpr int BAND_Y         = 402, BAND_H = 116;               // banner()'s black band

constexpr uint32_t IDLE_SLEEP_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t HOLD_MS = 700;

// Front-light steps shared with the apps (PT4103 PWM duty, launcher_api.h).
static const uint8_t kLightDuty[4] = {0, 12, 90, 230};
static const char* const kLightName[4] = {"OFF", "LOW", "MED", "HIGH"};
static int lightLevel(uint8_t duty) {
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (abs((int)duty - (int)kLightDuty[i]) < abs((int)duty - (int)kLightDuty[best])) best = i;
    return best;
}

enum Screen : uint8_t { HOME, SETTINGS, SHEET_FLASH };

enum Action : uint8_t {
    ACT_NONE, ACT_BOOT, ACT_SETTINGS,
    ACT_TOUCHFLIP, ACT_LIGHT, ACT_LIGHT_DOWN, ACT_LIGHT_UP,
    ACT_SYSCHECK, ACT_SLEEP, ACT_FLASH, ACT_BACK,
    ACT_SHEET_CONFIRM, ACT_SHEET_CANCEL,
};

// Tap targets, registered while a screen draws so hit-testing and drawing
// cannot disagree. The `focusable` ones are what the buttons step through.
struct Hit { int16_t x, y, w, h; Action action; int8_t arg; bool focusable; };
static Hit    g_hits[16];
static int    g_hitCount = 0;
static Action g_focusAct = ACT_NONE;
static int    g_focusArg = 0;

static Screen   g_screen = HOME;
static bool     g_ready = false;
static char     g_notice[96] = {0};
static char     g_slotName[4][32];          // names drawn on HOME, for the "Starting" banner
static uint32_t g_lastInput = 0;
static uint32_t g_touchIgnoreUntil = 0;
static bool     s_bootWas = false, s_sideWas = false, s_bootHeld = false, s_sideHeld = false;

void setNotice(const char* msg) { strlcpy(g_notice, msg ? msg : "", sizeof g_notice); }

static void addHit(int x, int y, int w, int h, Action a, int arg = 0, bool focusable = true) {
    if (g_hitCount >= (int)(sizeof g_hits / sizeof g_hits[0])) return;
    g_hits[g_hitCount++] = Hit{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, a, (int8_t)arg, focusable};
}
static bool focused(Action a, int arg = 0) { return g_focusAct == a && g_focusArg == arg; }
static int focusIndex() {
    for (int i = 0; i < g_hitCount; ++i)
        if (g_hits[i].focusable && g_hits[i].action == g_focusAct && g_hits[i].arg == g_focusArg) return i;
    return -1;
}
static void focusNext() {
    if (g_hitCount == 0) return;
    int i = focusIndex();
    for (int k = 0; k < g_hitCount; ++k) {
        i = (i + 1) % g_hitCount;
        if (g_hits[i].focusable) { g_focusAct = g_hits[i].action; g_focusArg = g_hits[i].arg; return; }
    }
}

// --- chrome ------------------------------------------------------------------

// clock | TITLE | battery, closed by a 3 px rule. The clock only shows once an
// app has set the RTC (OpenTrailPaper from GPS, Paperback from NTP).
void statusBar(const char* title) {
    GFXcanvas8& g = gfx();
    g.fillRect(0, 0, W, STATUS_H, WHITE);
    int hh, mm;
    if (hw::clock(hh, mm)) {
        char t[8];
        snprintf(t, sizeof t, "%02d:%02d", hh, mm);
        text(CELL_PAD, 41, t, FONT_ITEM, BLACK);
    }
    labelCentered(W / 2, 41, title, FONT_ITEM, BLACK);
    const int pct = hw::batteryPercent();
    batteryIcon(W - CELL_PAD, 30, pct, hw::usbPowered());
    char p[8];
    if (pct >= 0) snprintf(p, sizeof p, "%d%%", pct); else strlcpy(p, "--%", sizeof p);
    textRight(W - CELL_PAD - 39 - 10, 41, p, FONT_ITEM, BLACK);
    rule(0, STATUS_H - RULE_HEAVY, W, RULE_HEAVY);
}

// Up to two lines of text in `w` pixels; whatever does not fit the second line
// is ellipsised.
static void textLines2(int x, int y, int w, const char* s, const GFXfont* font, int lineH, uint8_t color) {
    if (textWidth(s, font) <= w) { text(x, y, s, font, color); return; }
    char first[96];
    strlcpy(first, s, sizeof first);
    const char* rest = nullptr;
    for (char* sp = strrchr(first, ' '); sp; sp = strrchr(first, ' ')) {
        *sp = 0;
        if (textWidth(first, font) <= w) { rest = s + (sp - first) + 1; break; }
    }
    if (!rest) { textFit(x, y, s, font, w, color); return; }
    text(x, y, first, font, color);
    textFit(x, y + lineH, rest, font, w, color);
}

// LIST ROW (148 px): title in caps over a sentence-case subtitle, the pair
// centred as a block; a solid triangle at the right says "opens something";
// 2 px rule below. `inverted` is the focused row: black all the way across.
static void listRow(int y, const char* title, const char* sub, bool enabled, bool marker, bool inverted) {
    GFXcanvas8& g = gfx();
    uint8_t fg = enabled ? BLACK : DARK;
    if (inverted) { g.fillRect(0, y, W, ROW_H, BLACK); fg = WHITE; }
    char up[48];
    upper(up, sizeof up, title);
    const int textW = CONTENT_W - 2 * CELL_PAD - (marker ? MARKER_W + CELL_PAD : 0);
    const int titleH = capHeight(FONT_TITLE), subH = capHeight(FONT_BODY), gap = 14;
    const int top = y + (ROW_H - (titleH + gap + subH)) / 2;
    textFit(TEXT_X, top + titleH, up, FONT_TITLE, textW, fg);
    if (sub && sub[0]) textFit(TEXT_X, top + titleH + gap + subH, sub, FONT_BODY, textW, fg);
    if (marker) {
        const int mx = CONTENT_X + CONTENT_W - CELL_PAD - MARKER_W, cy = y + ROW_H / 2;
        g.fillTriangle(mx, cy - 14, mx, cy + 14, mx + MARKER_W, cy, fg);
    }
    if (!inverted) rule(0, y + ROW_H - RULE, W, RULE);
}

// SETTINGS ROW (108 px): caps label over a small subtitle; the caller draws the
// control at the right in the returned ink, `labelRoom` keeps the text clear of it.
static uint8_t denseRow(int y, const char* labelText, const char* sub, int labelRoom, bool marker, bool inverted) {
    GFXcanvas8& g = gfx();
    uint8_t fg = BLACK;
    if (inverted) { g.fillRect(0, y, W, DENSE_ROW_H, BLACK); fg = WHITE; }
    char up[32];
    upper(up, sizeof up, labelText);
    const int labelH = capHeight(FONT_ITEM), subH = capHeight(FONT_SMALL), gap = 10;
    const bool hasSub = sub && sub[0];
    const int top = y + (DENSE_ROW_H - (hasSub ? labelH + gap + subH : labelH)) / 2;
    textFit(TEXT_X, top + labelH, up, FONT_ITEM, labelRoom, fg);
    if (hasSub) textFit(TEXT_X, top + labelH + gap + subH, sub, FONT_SMALL, labelRoom, fg);
    if (marker) {
        const int mx = CONTENT_X + CONTENT_W - CELL_PAD - MARKER_W, cy = y + DENSE_ROW_H / 2;
        g.fillTriangle(mx, cy - 12, mx, cy + 12, mx + MARKER_W, cy, fg);
    }
    if (!inverted) rule(0, y + DENSE_ROW_H - RULE, W, RULE);
    return fg;
}

// SWITCH: two halves, the active one filled and carrying its word. It shows a
// position, so OFF is a legible state and not an empty box.
static void switchCtl(int x, int y, bool on, uint8_t fg, uint8_t bg) {
    GFXcanvas8& g = gfx();
    frame(x, y, SWITCH_W, SWITCH_H, RULE, fg);
    const int half = SWITCH_W / 2;
    const int fx = on ? x : x + half;
    g.fillRect(fx + RULE, y + RULE, half - RULE, SWITCH_H - 2 * RULE, fg);
    labelCentered(fx + half / 2, y + SWITCH_H / 2 + capHeight(FONT_LABEL) / 2, on ? "ON" : "OFF", FONT_LABEL, bg);
}

// STEPPER target: an 88 x 88 outlined square with a bar-drawn minus or plus.
static void stepBtn(int x, int cy, bool plus, uint8_t fg) {
    GFXcanvas8& g = gfx();
    const int y = cy - STEP_BTN / 2, cx = x + STEP_BTN / 2;
    frame(x, y, STEP_BTN, STEP_BTN, RULE, fg);
    g.fillRect(cx - 18, cy - 3, 36, 7, fg);
    if (plus) g.fillRect(cx - 3, cy - 18, 7, 36, fg);
}

// Full-width button (96 px). Solid when it is what SELECT would do, otherwise a
// 3 px outline. The hit rect is full-bleed: the margins count too.
static void wideButton(int y, const char* caption, bool solid, Action a, bool leftArrow = false) {
    GFXcanvas8& g = gfx();
    if (solid) g.fillRect(CONTENT_X, y, CONTENT_W, BTN_H, BLACK);
    else frame(CONTENT_X, y, CONTENT_W, BTN_H, 3, BLACK);
    const uint8_t ink = solid ? WHITE : BLACK;
    char up[24];
    upper(up, sizeof up, caption);
    const int base = y + BTN_H / 2 + capHeight(FONT_TITLE) / 2;
    if (leftArrow) {
        const int ax = CONTENT_X + 32, cy = y + BTN_H / 2;
        g.fillTriangle(ax + MARKER_W, cy - 14, ax + MARKER_W, cy + 14, ax, cy, ink);
        text(ax + MARKER_W + 20, base, up, FONT_TITLE, ink);
    } else {
        textCentered(W / 2, base, up, FONT_TITLE, ink);
    }
    addHit(0, y, W, BTN_H, a);
}

// The notice under the status bar (crash notes, failed starts): a black "!"
// square and up to two lines of text. Returns the height it used.
static int noticeBand(int y) {
    if (!g_notice[0]) return 0;
    GFXcanvas8& g = gfx();
    g.fillRect(CONTENT_X, y + 24, 48, 48, BLACK);
    textCentered(CONTENT_X + 24, y + 48 + capHeight(FONT_TITLE) / 2, "!", FONT_TITLE, WHITE);
    const int tx = CONTENT_X + 48 + CELL_PAD;
    textLines2(tx, y + 42, W - MARGIN - tx, g_notice, FONT_BODY, 28, BLACK);
    rule(0, y + NOTICE_H - RULE, W, RULE);
    return NOTICE_H;
}

// --- screens -----------------------------------------------------------------

// HOME: status band, notice, one row per OTA slot, and the SETTINGS row pinned
// above the footer so it is always in the same place. Nothing else: state and
// readouts live in SETTINGS.
static void drawHome() {
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    g_hitCount = 0;
    statusBar("LAUNCHER");
    int y = STATUS_H + noticeBand(STATUS_H);

    // Empty slots are shown, greyed, so the flash layout is visible; only valid
    // images are targets.
    registry::Slot slots[8];
    const int n = registry::scan(slots, 8);
    int shown = 0;
    for (int i = 0; i < n && shown < 3; ++i) {
        const registry::Slot& s = slots[i];
        if (s.index < 0) continue;
        char sub[96];
        if (s.valid) {
            snprintf(sub, sizeof sub, "%s%sslot %s%s", s.version, s.version[0] ? "  -  " : "",
                     registry::slotLabel(s.index), s.last ? "  -  last run" : "");
            listRow(y, s.name, sub, true, true, focused(ACT_BOOT, s.index));
            addHit(0, y, W, ROW_H, ACT_BOOT, s.index);
            if (s.index < 4) strlcpy(g_slotName[s.index], s.name, sizeof g_slotName[0]);
        } else {
            snprintf(sub, sizeof sub, "slot %s  -  tools/flash.py app to install", registry::slotLabel(s.index));
            listRow(y, "Empty slot", sub, false, false, false);
        }
        y += ROW_H;
        shown++;
    }
    if (shown == 0) {
        text(TEXT_X, y + 80, "No OTA slots in the partition table.", FONT_BODY, BLACK);
        y += ROW_H;
    }

    // Settings flows right after the app rows, so the home screen reads as one
    // list from the top: the apps, then the way into everything else.
    listRow(y, "Settings", "Touch flip, light, system check, sleep", true, true, focused(ACT_SETTINGS));
    addHit(0, y, W, ROW_H, ACT_SETTINGS);

    char ver[16];
    snprintf(ver, sizeof ver, "v%s", LAUNCHER_VERSION);
    text(MARGIN, FOOTER_BASE, ver, FONT_SMALL, DARK);
    textRight(W - MARGIN, FOOTER_BASE, "BOOT: next / hold: select     side: next / hold: back", FONT_SMALL, DARK);
}

// SETTINGS: five dense rows, the power and build lines, the BACK strip.
//   64 + 5 * 108 = 604; power line at 760, build line at 792; BACK 840..936.
static void drawSettings() {
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    g_hitCount = 0;
    statusBar("SETTINGS");
    int y = STATUS_H;
    const int switchRoom = SWITCH_X - TEXT_X - GUTTER;                     // 328
    const int stepRoom   = STEP_MINUS_X - 8 - TEXT_X - GUTTER;             // 160
    const int navRoom    = CONTENT_W - 2 * CELL_PAD - MARKER_W - CELL_PAD;  // 424

    {
        const uint8_t fg = denseRow(y, "Touch flip", "Rotate touch input 180 degrees", switchRoom, false, focused(ACT_TOUCHFLIP));
        switchCtl(SWITCH_X, y + (DENSE_ROW_H - SWITCH_H) / 2, registry::touchFlip(), fg, fg == WHITE ? BLACK : WHITE);
        addHit(0, y, W, DENSE_ROW_H, ACT_TOUCHFLIP);
        y += DENSE_ROW_H;
    }
    {
        // Stepper. The two squares step (clamped: "+" at HIGH wrapping to OFF
        // would read as a fault); the label side of the row cycles.
        const uint8_t fg = denseRow(y, "Light", "Shared with apps", stepRoom, false, focused(ACT_LIGHT));
        const int cy = y + DENSE_ROW_H / 2;
        stepBtn(STEP_MINUS_X, cy, false, fg);
        stepBtn(STEP_PLUS_X, cy, true, fg);
        textCentered(STEP_VALUE_CX, cy + capHeight(FONT_ITEM) / 2, kLightName[lightLevel(launcher::frontLight())], FONT_ITEM, fg);
        addHit(0, y, STEP_MINUS_X - 8, DENSE_ROW_H, ACT_LIGHT);
        addHit(STEP_MINUS_X - 8, y, STEP_BTN + 16, DENSE_ROW_H, ACT_LIGHT_DOWN, 0, false);
        addHit(STEP_PLUS_X - 8, y, W - (STEP_PLUS_X - 8), DENSE_ROW_H, ACT_LIGHT_UP, 0, false);
        y += DENSE_ROW_H;
    }
    denseRow(y, "System check", "Tests every chip; results on screen and serial", navRoom, true, focused(ACT_SYSCHECK));
    addHit(0, y, W, DENSE_ROW_H, ACT_SYSCHECK);
    y += DENSE_ROW_H;
    denseRow(y, "Sleep now", "Deep sleep; BOOT wakes into this menu", navRoom, true, focused(ACT_SLEEP));
    addHit(0, y, W, DENSE_ROW_H, ACT_SLEEP);
    y += DENSE_ROW_H;
    denseRow(y, "Flash mode", "USB download mode for tools/flash.py", navRoom, true, focused(ACT_FLASH));
    addHit(0, y, W, DENSE_ROW_H, ACT_FLASH);

    {
        const int pct = hw::batteryPercent();
        const unsigned mv = hw::batteryMillivolts();
        char pw[96], pctS[8], mvS[16];
        if (pct >= 0) snprintf(pctS, sizeof pctS, "%d%%", pct); else strlcpy(pctS, "--%", sizeof pctS);
        if (mv) snprintf(mvS, sizeof mvS, "  -  %u mV", mv); else mvS[0] = 0;
        snprintf(pw, sizeof pw, "Battery %s%s  -  %s", pctS, mvS, hw::usbPowered() ? "USB power" : "on battery");
        textCentered(W / 2, POWER_BASE, pw, FONT_SMALL, DARK);
    }
    char b[96];
    snprintf(b, sizeof b, "%s v%s  -  built %s  -  IDF %s", LAUNCHER_NAME, LAUNCHER_VERSION, __DATE__, esp_get_idf_version());
    if (textWidth(b, FONT_SMALL) <= CONTENT_W) textCentered(W / 2, BUILD_BASE, b, FONT_SMALL, DARK);
    else textFit(CONTENT_X, BUILD_BASE, b, FONT_SMALL, CONTENT_W, DARK);

    wideButton(BACK_Y, "Back", focused(ACT_BACK), ACT_BACK, true);
}

// Confirmation sheet over SETTINGS: download mode leaves the device dark and
// waiting, so a stray tap must not get there. The scrim tones the body only;
// the status band stays readable.
static void drawSheet() {
    drawSettings();
    g_hitCount = 0;                      // the sheet has the input
    GFXcanvas8& g = gfx();
    tone50(0, STATUS_H, W, SHEET_Y - STATUS_H);
    g.fillRect(0, SHEET_Y, W, H - SHEET_Y, WHITE);
    rule(0, SHEET_Y, W, 6);
    int y = SHEET_Y + 6 + 40;
    label(CONTENT_X, y, "FLASH MODE", FONT_LABEL, BLACK);
    y += 8 + capHeight(FONT_HERO);
    text(CONTENT_X, y, "DOWNLOAD MODE?", FONT_HERO, BLACK);
    y += 20 + capHeight(FONT_BODY);
    text(CONTENT_X, y, "Reboots into the USB download mode", FONT_BODY, BLACK);  y += 28;
    text(CONTENT_X, y, "and waits for tools/flash.py.", FONT_BODY, BLACK);       y += 28;
    text(CONTENT_X, y, "The screen keeps this image;", FONT_BODY, BLACK);        y += 28;
    text(CONTENT_X, y, "press RESET to leave without flashing.", FONT_BODY, BLACK);
    wideButton(SHEET_BTN1_Y, "Flash", focused(ACT_SHEET_CONFIRM), ACT_SHEET_CONFIRM);
    wideButton(SHEET_BTN2_Y, "Cancel", focused(ACT_SHEET_CANCEL), ACT_SHEET_CANCEL);
    addHit(0, 0, W, SHEET_Y, ACT_SHEET_CANCEL, 0, false);   // a tap on the scrim cancels
}

void banner(const char* title, const char* line) {
    if (!display::begin()) return;
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    g.fillRect(0, BAND_Y, W, BAND_H, BLACK);
    char up[32];
    upper(up, sizeof up, title);
    textCentered(W / 2, BAND_Y + BAND_H / 2 + capHeight(FONT_HERO) / 2, up, FONT_HERO, WHITE);
    if (line && line[0]) textCentered(W / 2, BAND_Y + BAND_H + 48, line, FONT_BODY, BLACK);
    paint();
}

void draw() {
    if (!g_ready) return;
    switch (g_screen) {
        case HOME:        drawHome(); break;
        case SETTINGS:    drawSettings(); break;
        case SHEET_FLASH: drawSheet(); break;
    }
    paint();
}

// --- navigation ----------------------------------------------------------------

static void defaultFocus() {
    g_focusAct = ACT_NONE;
    g_focusArg = 0;
    if (g_screen == HOME) {
        // The app a cold boot would start is the natural first pick.
        const int last = registry::lastSlot();
        if (last >= 0 && registry::otaPartition(last)) { g_focusAct = ACT_BOOT; g_focusArg = last; }
    } else if (g_screen == SHEET_FLASH) {
        g_focusAct = ACT_SHEET_CANCEL;   // the safe default for a blind SELECT
    }
}

static void showScreen(Screen s) {
    g_screen = s;
    defaultFocus();
    draw();
}

static void setLight(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    launcher::setFrontLight(kLightDuty[level]);
    hw::backlight(kLightDuty[level]);
    draw();
}

static void activate(Action a, int arg) {
    switch (a) {
    case ACT_BOOT: {
        const char* name = (arg >= 0 && arg < 4 && g_slotName[arg][0]) ? g_slotName[arg] : registry::slotLabel(arg);
        banner("Starting", name);
        hw::backlight(0);                    // the app brings the light back up itself
        registry::boot(arg, esp_reset_reason());
        hw::backlight(launcher::frontLight());
        setNotice("Could not start that slot.");
        showScreen(HOME);
        break;
    }
    case ACT_SETTINGS:  showScreen(SETTINGS); break;
    case ACT_BACK:      showScreen(HOME); break;
    case ACT_TOUCHFLIP: {
        const bool on = !registry::touchFlip();
        registry::setTouchFlip(on);
        hw::setTouchFlip(on);
        draw();
        break;
    }
    case ACT_LIGHT:      setLight((lightLevel(launcher::frontLight()) + 1) % 4); break;
    case ACT_LIGHT_DOWN: setLight(lightLevel(launcher::frontLight()) - 1); break;
    case ACT_LIGHT_UP:   setLight(lightLevel(launcher::frontLight()) + 1); break;
    case ACT_SYSCHECK:
        syscheck::run(true);
        g_touchIgnoreUntil = millis() + 800;   // the tap that ended the check must not land here
        draw();
        break;
    case ACT_SLEEP:
        banner("Sleeping", "Press BOOT to wake");
        hw::deepSleep();
        break;
    case ACT_FLASH:        showScreen(SHEET_FLASH); break;
    case ACT_SHEET_CANCEL: showScreen(SETTINGS); break;
    case ACT_SHEET_CONFIRM:
        banner("Download mode", "Waiting for tools/flash.py  -  RESET to leave");
        hw::rebootToDownloadMode();
        break;
    default: break;
    }
}

static void onTap(int x, int y) {
    for (int i = g_hitCount - 1; i >= 0; --i) {
        const Hit h = g_hits[i];
        if (x < h.x || x >= h.x + h.w || y < h.y || y >= h.y + h.h) continue;
        if (h.focusable) { g_focusAct = h.action; g_focusArg = h.arg; }
        activate(h.action, h.arg);
        return;
    }
}

static void onBack() {
    switch (g_screen) {
        case SHEET_FLASH: showScreen(SETTINGS); break;
        case SETTINGS:    showScreen(HOME); break;
        case HOME:        break;
    }
}

static void onSelect() {
    const int i = focusIndex();
    if (i >= 0) { activate(g_hits[i].action, g_hits[i].arg); return; }
    focusNext();   // nothing focused yet: the first hold just picks the first item
    draw();
}

static void onNext() {
    focusNext();
    draw();
}

bool begin() {
    if (g_ready) return true;
    if (!display::begin()) return false;
    g_ready = true;
    hw::touchBegin();
    // A button that is already down (the menu was forced by holding it) must
    // not count as a press once the menu is up.
    s_bootWas = s_bootHeld = hw::bootButtonPressed();
    s_sideWas = s_sideHeld = hw::sideButtonPressed();
    g_lastInput = millis();
    g_touchIgnoreUntil = millis() + 800;   // let the GT911 settle after the panel came up
    g_screen = HOME;
    defaultFocus();
    draw();
    Serial.println("[launcher] menu");
    return true;
}

bool show(const char* name) {
    if (!begin()) return false;
    if (!name || !*name || !strcasecmp(name, "home")) { showScreen(HOME); return true; }
    if (!strcasecmp(name, "settings")) { showScreen(SETTINGS); return true; }
    return false;
}

void poll() {
    if (!g_ready) return;
    const uint32_t now = millis();

    // Touch: two agreeing reads start a press, two empty reads end it, and the
    // tap fires on release at the last position - one tap per release.
    static bool down = false;
    static int pressN = 0, releaseN = 0, lx = 0, ly = 0;
    static uint32_t lastTap = 0;
    int tx, ty;
    const bool pressed = hw::touchRead(tx, ty);   // also delivers the home-key callback
    if (now < g_touchIgnoreUntil) {
        down = false; pressN = releaseN = 0;
    } else if (pressed) {
        releaseN = 0;
        if (!down && ++pressN >= 2) down = true;
        lx = tx; ly = ty;
        g_lastInput = now;
    } else {
        pressN = 0;
        if (down && ++releaseN >= 2) {
            down = false;
            if (now - lastTap > 350) { lastTap = now; onTap(lx, ly); }
        }
    }
    if (hw::homeKeyPressed()) { g_lastInput = now; onBack(); }

    // BOOT: short press = next, held = select (acts when the hold time passes).
    static uint32_t bootAt = 0;
    const bool boot = hw::bootButtonPressed();
    if (boot && !s_bootWas) { bootAt = now; s_bootHeld = false; }
    if (boot && !s_bootHeld && now - bootAt >= HOLD_MS) { s_bootHeld = true; g_lastInput = now; onSelect(); }
    if (!boot && s_bootWas && !s_bootHeld) { g_lastInput = now; onNext(); }
    s_bootWas = boot;

    // Side button (an I2C read, so every 25 ms): short = next, held = back.
    static uint32_t sidePoll = 0, sideAt = 0;
    if (now - sidePoll >= 25) {
        sidePoll = now;
        const bool side = hw::sideButtonPressed();
        if (side && !s_sideWas) { sideAt = now; s_sideHeld = false; }
        if (side && !s_sideHeld && now - sideAt >= HOLD_MS) { s_sideHeld = true; g_lastInput = now; onBack(); }
        if (!side && s_sideWas && !s_sideHeld) { g_lastInput = now; onNext(); }
        s_sideWas = side;
    }

    if (now - g_lastInput > IDLE_SLEEP_MS && !hw::usbPowered()) {
        banner("Sleeping", "Press BOOT to wake");
        hw::deepSleep();
    }
}

}  // namespace ui
