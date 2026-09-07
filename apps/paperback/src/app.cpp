#include "app.h"
#include "app_internal.h"

#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font.h"
#include "hn.h"
#include "hwio.h"
#include "launcher_api.h"
#include "library.h"
#include "settings.h"
#include "ui.h"

using namespace display;

namespace app {

Screen g_screen = LIBRARY;
Doc    g_doc;

// Reader geometry.
static const int READER_TOP = 58;
static const int READER_BOTTOM = 914;
static const int FOOTER_BASE = 942;
static const int BAR_Y = 952;
// Tap zones on a page: the top band opens the menu, the left 30 % goes back a
// page, everything else turns forward. 120 px is a thumb's width; the old 60
// was the header text alone and mostly missed.
static const int READER_MENU_ZONE = 120;

// Library state.
static int g_libPage = 0;
static int g_libSel = -1;          // highlighted grid item (buttons), -1 none
static const int GRID_PER_PAGE = 6;

// Menu / settings / chapters state.
static int g_settingsRow = -1;     // highlighted row for buttons
static int g_settingsPage = 0;     // 0 = text, 1 = device
static int g_chPage = 0;
static Screen g_settingsFrom = READER;

// Timing.
static uint32_t g_lastInput = 0;
static uint32_t g_lastTurn = 0;
static uint32_t g_turnsSinceScrub = 0;
static bool     g_progressDirty = false;
static uint32_t g_progressAt = 0;
static uint32_t g_ignoreTouchUntil = 0;

// State kept across deep sleep so the wake path knows what is on the glass.
// screen: READER = a book page is on the glass (book/bookId/offset say which),
// LIBRARY = the library with a sleep card, 0xFF = something else: scrub on wake.
struct RtcState { uint32_t magic; uint8_t screen; int16_t book; uint32_t bookId; uint32_t offset; int16_t rx, ry, rw, rh; };
static const uint32_t RTC_MAGIC = 0x50425231;   // "PBR1"
RTC_DATA_ATTR static RtcState g_rtc;

// ---- helpers ------------------------------------------------------------------------

static layout::Style readerStyle() {
    layout::Style st;
    const Settings& s = settings::get();
    if (s.fontFamily == 0) { st.body = font::SERIF; st.italic = font::SERIF_ITALIC; st.bold = font::SERIF_BOLD; }
    else { st.body = font::SANS; st.italic = font::SERIF_ITALIC; st.bold = font::SANS_BOLD; }
    st.px = settings::bodyPx();
    st.lineH = settings::lineHeight();
    st.colX = settings::marginPx();
    st.colW = W - 2 * st.colX;
    st.top = READER_TOP;
    st.bottom = READER_BOTTOM;
    st.justify = s.justify != 0;
    st.indent = (int)lroundf(st.px * 1.5f);
    st.paraGap = 0;
    return st;
}

static uint32_t pageOffset(uint32_t page) {
    if (!g_doc.open || g_doc.pages.count == 0) return 0;
    if (page >= g_doc.pages.count) page = g_doc.pages.count - 1;
    return layout::absOffset(g_doc.model, g_doc.pages.pages[page]);
}

static int percentNow() {
    if (!g_doc.open || g_doc.pages.count == 0) return 0;
    return (int)(((uint64_t)(g_doc.page + 1) * 100) / g_doc.pages.count);
}

static void computeTicks() {
    g_doc.tickCount = 0;
    const text::Model& m = g_doc.model;
    if (g_doc.pages.count < 2) return;
    for (uint32_t c = 0; c < m.chapterCount && g_doc.tickCount < 64; ++c) {
        const uint32_t pg = layout::pageForOffset(g_doc.pages, m, m.paras[m.chapters[c]].start);
        if (pg == 0) continue;
        g_doc.ticks[g_doc.tickCount++] = (uint16_t)((uint64_t)pg * 1000 / g_doc.pages.count);
    }
}

static void relayout(uint32_t keepOffset) {
    const layout::Style st = readerStyle();
    const uint32_t t0 = millis();
    layout::paginate(g_doc.model, st, g_doc.pages);
    g_doc.page = layout::pageForOffset(g_doc.pages, g_doc.model, keepOffset);
    computeTicks();
    Serial.printf("[reader] %u pages in %lu ms (%u paragraphs, %u chapters, glyph cache %u KB)\n",
                  (unsigned)g_doc.pages.count, (unsigned long)(millis() - t0), (unsigned)g_doc.model.count,
                  (unsigned)g_doc.model.chapterCount, (unsigned)(font::cacheBytes() / 1024));
}

static void flushProgress(bool force) {
    if (!g_progressDirty) return;
    if (!force && millis() - g_progressAt < 4000) return;
    g_progressDirty = false;
    g_progressAt = millis();
    if (g_doc.open && g_doc.book >= 0) {
        const bool last = g_doc.pages.count && g_doc.page + 1 >= g_doc.pages.count;
        library::saveProgress(g_doc.book, pageOffset(g_doc.page), (uint8_t)percentNow(),
                              library::at(g_doc.book).finished && last);
    }
    settings::save();
}

static void markProgress() { g_progressDirty = true; }

void closeDoc() {
    if (!g_doc.open) return;
    flushProgress(true);
    layout::release(g_doc.pages);
    text::release(g_doc.model);
    if (g_doc.ownsText && g_doc.text) heap_caps_free((void*)g_doc.text);
    library::close();
    g_doc = Doc();
}

bool openText(const char* txt, uint32_t len, bool ownsText, const char* title, const char* subtitle, uint32_t startOffset) {
    g_doc.text = txt;
    g_doc.len = len;
    g_doc.ownsText = ownsText;
    strlcpy(g_doc.title, title ? title : "", sizeof g_doc.title);
    strlcpy(g_doc.subtitle, subtitle ? subtitle : "", sizeof g_doc.subtitle);
    const uint32_t t0 = millis();
    if (!text::build(g_doc.model, txt, len)) {
        Serial.println("[reader] empty document");
        if (ownsText) heap_caps_free((void*)txt);
        g_doc = Doc();
        return false;
    }
    Serial.printf("[reader] model: %u paragraphs, ~%u words, %u chapters in %lu ms\n", (unsigned)g_doc.model.count,
                  (unsigned)g_doc.model.words, (unsigned)g_doc.model.chapterCount, (unsigned long)(millis() - t0));
    g_doc.open = true;
    g_doc.jumpFrom = -1;
    relayout(startOffset >= len ? 0 : startOffset);
    return true;
}

bool openBook(int idx) {
    if (idx < 0 || idx >= library::count()) return false;
    closeDoc();
    const library::Book& b = library::at(idx);
    if (b.src == library::SDCARD && b.size > 300 * 1024) ui::toast("Opening\xE2\x80\xA6", b.title);
    const char* txt; uint32_t len;
    if (!library::open(idx, txt, len)) return false;
    const uint32_t start = b.finished ? 0 : b.offset;
    if (!openText(txt, len, false, b.title, b.author, start)) return false;
    g_doc.book = idx;
    library::touch(idx);
    if (b.finished) library::saveProgress(idx, 0, 0, false);
    return true;
}

void setScreen(Screen s) { g_screen = s; }

// ---- reader -----------------------------------------------------------------------------

static void drawReaderChrome() {
    // Header: title, status.
    font::drawFit(ui::MARGIN, 32, g_doc.title, font::SANS, 15, W - 2 * ui::MARGIN - 150, DARK);
    ui::status(W - ui::MARGIN, 34);
    // Footer.
    char left[48], right[64];
    snprintf(left, sizeof left, "%u of %u", (unsigned)(g_doc.page + 1), (unsigned)g_doc.pages.count);
    font::draw(ui::MARGIN, FOOTER_BASE, left, font::SANS, 15, DARK);
    const float spp = settings::get().secPerPage;
    const uint32_t pagesLeft = g_doc.pages.count - g_doc.page - 1;
    if (spp > 0 && pagesLeft > 0) {
        char dur[32]; ui::formatDuration(dur, sizeof dur, spp * pagesLeft);
        snprintf(right, sizeof right, "%d%%  \xC2\xB7  %s left", percentNow(), dur);
    } else {
        snprintf(right, sizeof right, "%d%%", percentNow());
    }
    font::drawRight(W - ui::MARGIN, FOOTER_BASE, right, font::SANS, 15, DARK);
    const float frac = g_doc.pages.count ? (float)(g_doc.page + 1) / g_doc.pages.count : 0.f;
    ui::progressBar(ui::MARGIN, BAR_Y, W - 2 * ui::MARGIN, 3, frac, g_doc.ticks, g_doc.tickCount);
}

void drawReader(bool paintNow) {
    fill(WHITE);
    ui::hitsClear();
    const layout::Style st = readerStyle();
    static layout::Page* page = nullptr;
    if (!page) page = (layout::Page*)heap_caps_malloc(sizeof(layout::Page), MALLOC_CAP_SPIRAM);
    if (g_doc.pages.count) {
        const uint32_t t0 = millis();
        layout::compose(g_doc.model, st, g_doc.pages.pages[g_doc.page], *page);
        layout::render(g_doc.model, st, *page);
        Serial.printf("[reader] page %u/%u drawn in %lu ms\n", (unsigned)(g_doc.page + 1), (unsigned)g_doc.pages.count,
                      (unsigned long)(millis() - t0));
    }
    drawReaderChrome();
    g_screen = READER;
    if (paintNow) paintScreen();
}

void paintScreen() {
    setFast(g_screen == READER && settings::get().fastTurn);
    const int every = settings::refreshEvery();
    if (g_screen == READER && every > 0 && g_turnsSinceScrub >= (uint32_t)every) {
        scrub();
        g_turnsSinceScrub = 0;
    }
    paint();
}

void enterReader() {
    if (!(settings::get().hints & 1)) { g_screen = HINT; extern void drawHint(); drawHint(); return; }
    drawReader();
}

static void turnedForward() {
    const uint32_t now = millis();
    if (g_lastTurn) {
        const float dt = (now - g_lastTurn) / 1000.f;
        if (dt >= 2.5f && dt <= 180.f) {
            Settings& s = settings::get();
            s.secPerPage = s.secPerPage > 0 ? s.secPerPage * 0.85f + dt * 0.15f : dt;
        }
    }
    g_lastTurn = now;
}

extern void drawEnd();

bool nextPage() {
    if (!g_doc.open) return false;
    if (g_doc.page + 1 >= g_doc.pages.count) {
        if (g_doc.book >= 0) library::saveProgress(g_doc.book, pageOffset(g_doc.page), 100, true);
        drawEnd();
        return true;
    }
    turnedForward();
    g_doc.page++;
    g_turnsSinceScrub++;
    markProgress();
    drawReader();
    return true;
}

bool prevPage() {
    if (!g_doc.open) return false;
    if (g_doc.page == 0) return false;
    g_doc.page--;
    g_turnsSinceScrub++;
    g_lastTurn = millis();
    markProgress();
    drawReader();
    return true;
}

bool gotoPage(int page1) {
    if (!g_doc.open || page1 < 1 || (uint32_t)page1 > g_doc.pages.count) return false;
    g_doc.jumpFrom = (int)g_doc.page;
    g_doc.page = (uint32_t)(page1 - 1);
    g_lastTurn = millis();
    g_turnsSinceScrub = 1000;   // a jump gets a clean panel
    markProgress();
    drawReader();
    return true;
}

bool gotoPercent(int pct) {
    if (!g_doc.open || g_doc.pages.count == 0) return false;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    uint32_t page = (uint32_t)((uint64_t)pct * g_doc.pages.count / 100);
    if (page >= g_doc.pages.count) page = g_doc.pages.count - 1;
    return gotoPage((int)page + 1);
}

void refreshScreen() {
    g_turnsSinceScrub = 1000;
    if (g_screen == READER) drawReader();
    else { scrub(); paint(); }
}

void settingsChanged(bool doRelayout) {
    hwio::frontLight(settings::lightDuty());
    launcher::setFrontLight(settings::lightDuty());   // the launcher and other apps follow
    if (doRelayout && g_doc.open) {
        const uint32_t keep = pageOffset(g_doc.page);
        relayout(keep);
        font::flushCache();      // sizes changed; drop stale bitmaps
        g_turnsSinceScrub = 1000;
    }
    settings::save();
}

// ---- hint (first open) ---------------------------------------------------------------------

void drawHint() {
    fill(WHITE);
    ui::hitsClear();
    // Zone diagram in a page-shaped frame. 420 x 560 stands for the 540 x 960
    // panel (x0.58), so the 120 px menu band is 70 px tall here and the 30 %
    // back zone 126 px wide - the picture is to scale with the real zones.
    const int fx = 60, fy = 90, fw = W - 120, fh = 560;
    const int bandH = 70;
    const int leftW = (int)(fw * 0.3f);
    roundRect(fx, fy, fw, fh, 12, BLACK, 2);
    for (int y = fy + bandH; y < fy + fh - 6; y += 6) pixel(fx + leftW, y, DARK);
    for (int x = fx + 6; x < fx + fw - 6; x += 6) pixel(x, fy + bandH, DARK);
    font::drawCentered(fx + fw / 2, fy + 44, "Tap the top band for the menu", font::SANS_BOLD, 17, BLACK);
    ui::chevron(fx + leftW / 2, fy + fh / 2 - 30, 14, false, BLACK, 3);
    font::drawCentered(fx + leftW / 2, fy + fh / 2 + 20, "Back", font::SANS_BOLD, 17, BLACK);
    font::drawCentered(fx + leftW / 2, fy + fh / 2 + 44, "a page", font::SANS, 15, DARK);
    ui::chevron(fx + leftW + (fw - leftW) / 2, fy + fh / 2 - 30, 14, true, BLACK, 3);
    font::drawCentered(fx + leftW + (fw - leftW) / 2, fy + fh / 2 + 20, "Next page", font::SANS_BOLD, 17, BLACK);
    font::drawCentered(fx + leftW + (fw - leftW) / 2, fy + fh / 2 + 44, "tap or swipe left", font::SANS, 15, DARK);
    font::drawCentered(W / 2, 700, "Buttons work too", font::SANS_BOLD, 19, BLACK);
    font::drawCentered(W / 2, 734, "BOOT: next page   \xC2\xB7   side button: previous page", font::SANS, 15, DARK);
    font::drawCentered(W / 2, 760, "Hold BOOT for one second to return to the launcher.", font::SANS, 15, DARK);
    font::drawCentered(W / 2, 792, "Holding a finger on the page also opens the menu.", font::SANS, 15, DARK);
    // Full-width, 96 px: the one thing to hit on this screen (840..936).
    ui::button(ui::MARGIN, 840, ui::CONTENT_W, 96, "Start reading", true, H_HINT_OK);
    g_screen = HINT;
    paint();
}

// ---- end of book ------------------------------------------------------------------------------

void drawEnd() {
    fill(WHITE);
    ui::hitsClear();
    fillCircle(W / 2, 300, 6, BLACK);
    hline(W / 2 - 120, 300, 100, BLACK);
    hline(W / 2 + 20, 300, 100, BLACK);
    font::drawCentered(W / 2, 400, "The End", font::SERIF_BOLD, 48, BLACK);
    font::drawWrapped(60, 460, W - 120, g_doc.title, font::SERIF_ITALIC, 24, 30, 2, BLACK, 1);
    char line[80];
    snprintf(line, sizeof line, "%u pages", (unsigned)g_doc.pages.count);
    font::drawCentered(W / 2, 560, line, font::SANS, 17, DARK);
    const bool web = g_doc.book < 0;
    // Two full-width 88 px buttons: 640..728 and 744..832.
    ui::button(ui::MARGIN, 640, ui::CONTENT_W, ui::TOUCH_MIN, web ? "Back to Hacker News" : "Back to library", true, H_END_LIBRARY);
    ui::button(ui::MARGIN, 744, ui::CONTENT_W, ui::TOUCH_MIN, "Read again", false, H_END_AGAIN);
    g_screen = END;
    paint();
}

// ---- library ---------------------------------------------------------------------------------------

static int gridPages() {
    const int n = library::count();
    return n <= 0 ? 1 : (n + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
}

void drawLibrary() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Library");

    // Vertical budget (header 84, bar 856): hero 100..276, shelf label 318,
    // grid 332..820, hint line 842, action bar 856..944.
    const int heroY = 100, heroH = 176, heroCoverW = 126;
    const int recent = library::mostRecent();
    if (recent >= 0) {
        const library::Book& b = library::at(recent);
        ui::cover(ui::MARGIN, heroY, heroCoverW, heroH, b, false);
        const int tx = ui::MARGIN + heroCoverW + 22;   // 172
        const int tw = W - ui::MARGIN - tx;            // 344
        ui::sectionLabel(tx, heroY + 14, b.finished ? "Finished" : "Continue reading");
        const int lines = font::drawWrapped(tx, heroY + 50, tw, b.title, font::SERIF_BOLD, 24, 29, 2, BLACK, 0);
        font::drawFit(tx, heroY + 50 + lines * 29 + 2, b.author, font::SANS, 16, tw, DARK);
        const int py = heroY + 122;
        ui::progressBar(tx, py, tw, 6, b.finished ? 1.f : b.percent / 100.f);
        char line[96];
        if (b.finished) snprintf(line, sizeof line, "Read again from the start");
        else if (b.percent == 0) snprintf(line, sizeof line, "Not started yet");
        else snprintf(line, sizeof line, "%d%% read", b.percent);
        const float spp = settings::get().secPerPage;
        if (!b.finished && b.percent > 0 && spp > 0 && b.size > 0) {
            // Rough estimate from bytes: pages are re-laid out on open.
            const float pagesLeft = (b.size - b.offset) / 1500.f;
            char dur[32]; ui::formatDuration(dur, sizeof dur, pagesLeft * spp);
            const size_t k = strlen(line);
            snprintf(line + k, sizeof line - k, "  \xC2\xB7  about %s left", dur);
        }
        font::drawFit(tx, py + 28, line, font::SANS, 14, tw, DARK);
        ui::hit(0, heroY - 6, W, heroH + 12, H_HERO, recent);
    } else {
        roundRect(ui::MARGIN, heroY, ui::CONTENT_W, heroH, 12, LIGHT, 1);
        font::draw(ui::MARGIN + 22, heroY + 46, "Welcome to Paperback", font::SERIF_BOLD, 26, BLACK);
        font::drawWrapped(ui::MARGIN + 22, heroY + 84, ui::CONTENT_W - 44,
                          "Tap a book to start reading. Your place is remembered for every book.",
                          font::SANS, 16, 22, 2, DARK, 0);
        font::drawWrapped(ui::MARGIN + 22, heroY + 132, ui::CONTENT_W - 44,
                          "Add your own: copy .txt files into a /books folder on the SD card.",
                          font::SANS, 15, 20, 2, DARK, 0);
    }

    // Shelf: 3 x 2 covers of 140 x 176, progress bar under the cover, then a
    // two-line title and the author (caption 66 px). Row pitch 248: covers at
    // 332 and 580, the second caption ends at 821. Hit rects are the cover
    // plus caption with 8 px of air: 156 x 248, touching between the rows.
    const int n = library::count();
    const int pages = gridPages();
    if (g_libPage >= pages) g_libPage = pages - 1;
    if (g_libPage < 0) g_libPage = 0;
    char lbl[48];
    snprintf(lbl, sizeof lbl, "All books  \xC2\xB7  %d", n);
    ui::sectionLabel(ui::MARGIN, 318, lbl);
    if (pages > 1) {
        char pg[16]; snprintf(pg, sizeof pg, "Page %d of %d", g_libPage + 1, pages);
        font::drawRight(W - ui::MARGIN, 318, pg, font::SANS, 13, DARK);
    }
    const int cw = 140, ch = 176, gapX = (ui::CONTENT_W - 3 * cw) / 2, rowPitch = ch + 72;
    const int startIdx = g_libPage * GRID_PER_PAGE;
    for (int i = 0; i < GRID_PER_PAGE; ++i) {
        const int idx = startIdx + i;
        if (idx >= n) break;
        const library::Book& b = library::at(idx);
        const int col = i % 3, row = i / 3;
        const int x = ui::MARGIN + col * (cw + gapX);
        const int y = 332 + row * rowPitch;
        ui::cover(x, y, cw, ch, b, g_libSel == i);
        if (b.percent > 0 || b.finished) ui::progressBar(x, y + ch + 6, cw, 3, b.finished ? 1.f : b.percent / 100.f);
        const int lines = font::drawWrapped(x, y + ch + 26, cw, b.title, font::SANS_BOLD, 14, 18, 2, BLACK, 0);
        if (b.author[0]) font::drawFit(x, y + ch + 26 + lines * 18, b.author, font::SANS, 12, cw, DARK);
        ui::hit(x - 8, y - 8, cw + 16, ch + 72, H_BOOK, idx);
    }

    // One small line above the bar, then the bar itself: two icon buttons and
    // the pager, 140 + 140 + 88 + 88 with 12 px gutters = 492.
    char foot[96];
    if (library::sdMounted()) snprintf(foot, sizeof foot, "%d on the SD card  \xC2\xB7  hold BOOT for the launcher", library::sdCount());
    else snprintf(foot, sizeof foot, "No SD card  \xC2\xB7  hold BOOT for the launcher");
    font::draw(ui::MARGIN, 842, foot, font::SANS, 13, DARK);
    const int bx = ui::MARGIN, bw = (ui::CONTENT_W - 3 * ui::GUTTER - 2 * ui::TOUCH_MIN) / 2;   // 140
    ui::iconButton(bx, ui::BAR_Y, bw, ui::BAR_H, ui::ICON_HN, "Hacker News", H_HN_BANNER);
    ui::iconButton(bx + bw + ui::GUTTER, ui::BAR_Y, bw, ui::BAR_H, ui::ICON_GEAR, "Settings", H_SETTINGS);
    const int px = bx + 2 * (bw + ui::GUTTER);   // 328
    ui::chevronButton(px, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, false, H_LIB_PREV, 0, g_libPage > 0);
    ui::chevronButton(px + ui::TOUCH_MIN + ui::GUTTER, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, true, H_LIB_NEXT, 0, g_libPage + 1 < pages);
    g_screen = LIBRARY;
    paint();
}

void goToLibrary() {
    closeDoc();
    g_libSel = -1;
    drawLibrary();
}

// ---- book menu ------------------------------------------------------------------------------------------

static void drawMenu() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Reading", "Back", H_BACK);
    const bool web = g_doc.book < 0;

    // Book block 100..268 at most: cover 100 x 140 (or the HN mark), title up
    // to three lines, subtitle two, one credit line.
    int y = 100;
    if (!web) ui::cover(ui::MARGIN, y, 100, 140, library::at(g_doc.book), false);
    else ui::hnLogo(ui::MARGIN, y, 64);
    const int tx = ui::MARGIN + (web ? 84 : 122);
    const int tw = W - ui::MARGIN - tx;
    const int lines = font::drawWrapped(tx, y + 30, tw, g_doc.title, font::SERIF_BOLD, 24, 29, 3, BLACK, 0);
    const int subLines = font::drawWrapped(tx, y + 30 + lines * 29 + 4, tw, g_doc.subtitle, font::SANS, 15, 19, 2, DARK, 0);
    if (!web) {
        const library::Book& b = library::at(g_doc.book);
        char src[64]; snprintf(src, sizeof src, "%s%s%u", b.credit, b.year ? " \xC2\xB7 " : "", (unsigned)b.year);
        if (!b.year) snprintf(src, sizeof src, "%s", b.credit);
        font::drawFit(tx, y + 30 + lines * 29 + 4 + subLines * 19 + 6, src, font::SANS, 13, tw, DARK);
    }

    // Progress: page line at 284, chapter at 310, then the jump strip 322..410
    // - an 88 px target with the thin bar centred in it. The strip is the
    // control; the bar alone was a 10 px line nobody could hit.
    y = 284;
    char line[96];
    snprintf(line, sizeof line, "Page %u of %u  \xC2\xB7  %d%%", (unsigned)(g_doc.page + 1), (unsigned)g_doc.pages.count, percentNow());
    font::draw(ui::MARGIN, y, line, font::SANS_BOLD, 17, BLACK);
    const float spp = settings::get().secPerPage;
    const uint32_t left = g_doc.pages.count - g_doc.page - 1;
    if (spp > 0 && left > 0) {
        char dur[32]; ui::formatDuration(dur, sizeof dur, spp * left);
        snprintf(line, sizeof line, "about %s left", dur);
        font::drawRight(W - ui::MARGIN, y, line, font::SANS, 15, DARK);
    }
    const int ch = text::chapterForOffset(g_doc.model, pageOffset(g_doc.page));
    if (ch >= 0) {
        char t[120]; text::chapterTitle(g_doc.model, (uint32_t)ch, t, sizeof t);
        font::drawFit(ui::MARGIN, y + 26, t, font::SERIF_ITALIC, 17, ui::CONTENT_W, DARK);
    }
    const int stripY = 322, stripH = ui::TOUCH_MIN;
    roundRect(ui::MARGIN - 8, stripY, ui::CONTENT_W + 16, stripH, 10, LIGHT, 1);
    ui::progressBar(ui::MARGIN, stripY + 30, ui::CONTENT_W, 10,
                    g_doc.pages.count ? (float)(g_doc.page + 1) / g_doc.pages.count : 0.f, g_doc.ticks, g_doc.tickCount);
    font::drawCentered(W / 2, stripY + 74, "tap anywhere on this strip to jump there", font::SANS, 12, DARK);
    ui::hit(ui::MARGIN - 8, stripY, ui::CONTENT_W + 16, stripH, H_PROGRESS);

    // Buttons: 88 px rows on a 100 px pitch from 426 - full-width primary,
    // then 240 px pairs; the last row (826..914) is the front-light stepper.
    const int bx = ui::MARGIN, bw = (ui::CONTENT_W - ui::GUTTER) / 2, bh = ui::TOUCH_MIN, step = bh + ui::GUTTER;
    const int bx2 = bx + bw + ui::GUTTER;
    y = 426;
    ui::button(bx, y, ui::CONTENT_W, bh, "Continue reading", true, H_MENU_CONTINUE);
    y += step;
    ui::button(bx, y, bw, bh, "Chapters", false, H_MENU_CHAPTERS, 0, g_doc.model.chapterCount > 0);
    ui::button(bx2, y, bw, bh, "Text & display", false, H_MENU_SETTINGS);
    y += step;
    ui::button(bx, y, bw, bh, web ? "Hacker News" : "Library", false, H_MENU_LIBRARY);
    ui::button(bx2, y, bw, bh, "Sleep now", false, H_MENU_SLEEP);
    y += step;
    if (g_doc.jumpFrom >= 0) {
        snprintf(line, sizeof line, "Back to page %d", g_doc.jumpFrom + 1);
        ui::button(bx, y, bw, bh, line, false, H_MENU_BACKTO);
        ui::button(bx2, y, bw, bh, "Refresh screen", false, H_MENU_REFRESH);
    } else if (web && g_doc.hnStory >= 0) {
        const hn::Story& st = hn::story(g_doc.hnStory);
        const bool can = g_doc.isComments ? st.url[0] != 0 : true;
        ui::button(bx, y, bw, bh, g_doc.isComments ? "Read the article" : "Read the comments", false, H_MENU_SWITCH, 0, can);
        ui::button(bx2, y, bw, bh, "Refresh screen", false, H_MENU_REFRESH);
    } else {
        ui::button(bx, y, ui::CONTENT_W, bh, "Refresh screen", false, H_MENU_REFRESH);
    }
    y += step;

    // Front light: label + level name left, then [<] three bars [>] with 88 px
    // buttons. Levels are the shared off/low/medium/high steps (settings.h).
    ui::iconSun(ui::MARGIN + 12, y + 44, 11, BLACK);
    font::draw(ui::MARGIN + 36, y + 38, "Front light", font::SANS_BOLD, 17, BLACK);
    const int lvl = settings::get().light;
    font::draw(ui::MARGIN + 36, y + 64, settings::lightName(lvl), font::SANS, 16, DARK);
    const int plusX = W - ui::MARGIN - ui::TOUCH_MIN;              // 428
    const int barsX = plusX - 16 - (3 * 20 + 2 * 8);               // 336
    const int minusX = barsX - 16 - ui::TOUCH_MIN;                 // 232
    ui::chevronButton(minusX, y, ui::TOUCH_MIN, ui::TOUCH_MIN, false, H_LIGHT_MINUS, 0, lvl > 0);
    for (int i = 0; i < 3; ++i) {
        const int sx = barsX + i * 28;
        if (i < lvl) fillRoundRect(sx, y + 32, 20, 24, 3, BLACK); else roundRect(sx, y + 32, 20, 24, 3, DARK, 1);
    }
    ui::chevronButton(plusX, y, ui::TOUCH_MIN, ui::TOUCH_MIN, true, H_LIGHT_PLUS, 0, lvl < 3);

    ui::footer("Hold BOOT for one second to return to the launcher");
    g_screen = MENU;
    paint();
}

void showMenu() { if (g_doc.open) drawMenu(); }

// ---- settings ----------------------------------------------------------------------------------------------

static const int SETTINGS_ROWS = 10;
static const int SETTINGS_PER_PAGE = 5;   // page 0 "Text": rows 0..4, page 1 "Device": rows 5..9
static const char* rowLabel(int r) {
    static const char* names[SETTINGS_ROWS] = {"Font", "Size", "Line spacing", "Margins", "Alignment", "Page turn",
                                               "Full refresh", "Sleep after", "Time zone", "Front light"};
    return names[r];
}
static void rowValue(int r, char* out, size_t cap) {
    const Settings& s = settings::get();
    switch (r) {
        case 0: snprintf(out, cap, "%s", s.fontFamily ? "Inter (sans)" : "Literata (serif)"); break;
        case 1: snprintf(out, cap, "%d of %d", s.sizeIdx + 1, settings::SIZE_COUNT); break;
        case 2: snprintf(out, cap, "%s", s.spacing == 0 ? "Compact" : s.spacing == 1 ? "Normal" : "Relaxed"); break;
        case 3: snprintf(out, cap, "%s", s.margin == 0 ? "Narrow" : s.margin == 1 ? "Normal" : "Wide"); break;
        case 4: snprintf(out, cap, "%s", s.justify ? "Justified" : "Left"); break;
        case 5: snprintf(out, cap, "%s", s.fastTurn ? "Fast" : "Quality"); break;
        case 6: { static const char* v[4] = {"Off", "Every 5 pages", "Every 10 pages", "Every 20 pages"}; snprintf(out, cap, "%s", v[s.refreshIdx]); break; }
        case 7: { static const char* v[4] = {"5 min", "10 min", "30 min", "Never"}; snprintf(out, cap, "%s", v[s.sleepIdx]); break; }
        case 8: snprintf(out, cap, "%s", settings::tzName(s.tzIdx)); break;
        default: snprintf(out, cap, "%s", settings::lightName(s.light)); break;
    }
}
static bool rowStep(int r, int dir) {
    Settings& s = settings::get();
    auto cyc = [&](uint8_t& v, int n) { v = (uint8_t)(((int)v + dir + n) % n); };
    switch (r) {
        case 0: cyc(s.fontFamily, 2); return true;
        case 1: { int v = (int)s.sizeIdx + dir; if (v < 0 || v >= settings::SIZE_COUNT) return false; s.sizeIdx = (uint8_t)v; return true; }
        case 2: cyc(s.spacing, 3); return true;
        case 3: cyc(s.margin, 3); return true;
        case 4: cyc(s.justify, 2); return true;
        case 5: cyc(s.fastTurn, 2); return false;
        case 6: cyc(s.refreshIdx, 4); return false;
        case 7: cyc(s.sleepIdx, 4); return false;
        case 8: cyc(s.tzIdx, settings::TZ_COUNT); return false;
        default: { int v = (int)s.light + dir; if (v < 0 || v > 3) return false; s.light = (uint8_t)v; return false; }
    }
}

static void drawSettings() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Text & display", "Done", H_DONE);
    if (g_settingsRow >= 0) g_settingsPage = g_settingsRow / SETTINGS_PER_PAGE;
    if (g_settingsPage < 0 || g_settingsPage > 1) g_settingsPage = 0;

    // Both pages share one grid: a 200 px card at 100..300 (the live text
    // preview, or the device card), five 88 px rows 312..752, the button hint
    // at 820 and the page tabs 856..944.
    const int pvX = ui::MARGIN, pvY = 100, pvW = ui::CONTENT_W, pvH = 200;
    roundRect(pvX, pvY, pvW, pvH, 8, LIGHT, 1);
    if (g_settingsPage == 0) {
        if (g_doc.open && g_doc.pages.count) {
            layout::Style st = readerStyle();
            st.colX = pvX + 16; st.colW = pvW - 32; st.top = pvY + 12; st.bottom = pvY + pvH - 10;
            static layout::Page* page = nullptr;
            if (!page) page = (layout::Page*)heap_caps_malloc(sizeof(layout::Page), MALLOC_CAP_SPIRAM);
            layout::compose(g_doc.model, st, g_doc.pages.pages[g_doc.page], *page);
            layout::render(g_doc.model, st, *page);
        } else {
            font::drawCentered(W / 2, pvY + pvH / 2 + 8, "Open a book to preview the text style", font::SANS, 15, DARK);
        }
    } else {
        // Device card: version, battery, storage, network, memory - the things
        // a reader wants to know without leaving for the launcher.
        char l[96];
        ui::sectionLabel(pvX + 20, pvY + 34, "Paperback " APP_VERSION);
        const int pct = hwio::batteryPercent();
        const char* usb = hwio::usbPowered() ? "  \xC2\xB7  on USB power" : "";
        if (pct >= 0) snprintf(l, sizeof l, "Battery %d%%%s", pct, usb);
        else snprintf(l, sizeof l, "Battery level unknown%s", usb);
        font::drawFit(pvX + 20, pvY + 74, l, font::SANS, 16, pvW - 40, BLACK);
        if (library::sdMounted()) snprintf(l, sizeof l, "%d books  \xC2\xB7  %d on the SD card", library::count(), library::sdCount());
        else snprintf(l, sizeof l, "%d books  \xC2\xB7  no SD card", library::count());
        font::drawFit(pvX + 20, pvY + 104, l, font::SANS, 16, pvW - 40, BLACK);
        if (hn::configured()) snprintf(l, sizeof l, "Wi-Fi: %s%s", settings::get().wifiSsid, hn::online() ? "  \xC2\xB7  connected" : "");
        else snprintf(l, sizeof l, "Wi-Fi: not set up (console or SD card)");
        font::drawFit(pvX + 20, pvY + 134, l, font::SANS, 16, pvW - 40, BLACK);
        snprintf(l, sizeof l, "Free memory: %u KB internal, %u KB PSRAM",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        font::drawFit(pvX + 20, pvY + 164, l, font::SANS, 16, pvW - 40, DARK);
    }

    // Rows: label over value on the left (292 px of room), two 88 x 80 stepper
    // squares at 328 and 428 whose hit rects are the full 88 px row.
    const int rowH = ui::TOUCH_MIN;
    const int bxPrev = W - ui::MARGIN - 2 * rowH - ui::GUTTER;   // 328
    const int bxNext = W - ui::MARGIN - rowH;                    // 428
    int y = 312;
    for (int i = 0; i < SETTINGS_PER_PAGE; ++i, y += rowH) {
        const int r = g_settingsPage * SETTINGS_PER_PAGE + i;
        if (g_settingsRow == r) fillRoundRect(ui::MARGIN - 8, y + 2, bxPrev - ui::MARGIN, rowH - 4, 8, LIGHT);
        font::draw(ui::MARGIN, y + 38, rowLabel(r), font::SANS_BOLD, 17, BLACK);
        char v[40]; rowValue(r, v, sizeof v);
        font::drawFit(ui::MARGIN, y + 66, v, font::SANS, 16, bxPrev - ui::MARGIN - 16, DARK);
        ui::chevronButton(bxPrev, y + 4, rowH, rowH - 8, false, H_ROW_PREV, r, true);
        ui::chevronButton(bxNext, y + 4, rowH, rowH - 8, true, H_ROW_NEXT, r, true);
        hline(ui::MARGIN, y + rowH - 1, bxPrev - ui::MARGIN - 12, LIGHT);
    }
    font::drawCentered(W / 2, 820, "BOOT: next row   \xC2\xB7   side button: change value", font::SANS, 13, DARK);
    static const char* const kTabs[2] = {"Text", "Device"};
    ui::tabs(ui::MARGIN, ui::BAR_Y, ui::CONTENT_W, ui::BAR_H, kTabs, 2, g_settingsPage, H_SET_TAB);
    g_screen = SETTINGS;
    paint();
}

static void leaveSettings() {
    settings::save();
    if (g_settingsFrom == LIBRARY) drawLibrary();
    else drawMenu();
}

// ---- chapters ---------------------------------------------------------------------------------------------------

static const int CH_PER_PAGE = 8;

static void drawChapters() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Chapters", "Back", H_BACK);
    const text::Model& m = g_doc.model;
    const int n = (int)m.chapterCount;
    const int pages = n ? (n + CH_PER_PAGE - 1) / CH_PER_PAGE : 1;
    if (g_chPage >= pages) g_chPage = pages - 1;
    if (g_chPage < 0) g_chPage = 0;
    const int cur = text::chapterForOffset(m, pageOffset(g_doc.page));
    if (g_chPage == 0 && cur >= CH_PER_PAGE) g_chPage = cur / CH_PER_PAGE;
    // Rows 100..804 (8 x 88, full-width targets), pager 856..944.
    const int rowH = ui::TOUCH_MIN;
    int y = 100;
    for (int i = g_chPage * CH_PER_PAGE; i < n && i < (g_chPage + 1) * CH_PER_PAGE; ++i, y += rowH) {
        char t[140]; text::chapterTitle(m, (uint32_t)i, t, sizeof t);
        const uint32_t pg = layout::pageForOffset(g_doc.pages, m, m.paras[m.chapters[i]].start) + 1;
        if (i == cur) fillCircle(ui::MARGIN + 6, y + 44, 5, BLACK);
        font::drawFit(ui::MARGIN + 24, y + 52, t, i == cur ? font::SERIF_BOLD : font::SERIF, 20, ui::CONTENT_W - 90, BLACK);
        char p[12]; snprintf(p, sizeof p, "%u", (unsigned)pg);
        font::drawRight(W - ui::MARGIN, y + 52, p, font::SANS, 15, DARK);
        hline(ui::MARGIN, y + rowH - 1, ui::CONTENT_W, LIGHT);
        ui::hit(0, y, W, rowH, H_CHAPTER, i);
    }
    if (pages > 1) {
        ui::chevronButton(ui::MARGIN, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, false, H_CH_PREV, 0, g_chPage > 0);
        ui::chevronButton(W - ui::MARGIN - ui::TOUCH_MIN, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, true, H_CH_NEXT, 0, g_chPage + 1 < pages);
        char pg[24]; snprintf(pg, sizeof pg, "Page %d of %d", g_chPage + 1, pages);
        font::drawCentered(W / 2, ui::BAR_Y + 52, pg, font::SANS, 16, DARK);
    }
    g_screen = CHAPTERS;
    paint();
}

// ---- sleep / wake ----------------------------------------------------------------------------------------------------

void sleepNow() {
    flushProgress(true);
    settings::save();
    hn::disconnect();
    g_rtc.magic = RTC_MAGIC;
    if (g_screen == READER && g_doc.open && g_doc.book >= 0) {
        // Keep the page on the glass; swap the footer for a sleep note.
        g_rtc.screen = READER; g_rtc.book = (int16_t)g_doc.book; g_rtc.bookId = library::at(g_doc.book).id;
        g_rtc.offset = pageOffset(g_doc.page);
        g_rtc.rx = 0; g_rtc.ry = READER_BOTTOM + 4; g_rtc.rw = W; g_rtc.rh = H - (READER_BOTTOM + 4);
        fillRect(g_rtc.rx, g_rtc.ry, g_rtc.rw, g_rtc.rh, WHITE);
        ui::iconMoon(W / 2 - 128, FOOTER_BASE - 6, 8, BLACK);
        font::drawCentered(W / 2 + 8, FOOTER_BASE, "Sleeping  \xC2\xB7  press the BOOT button to wake", font::SANS, 14, DARK);
        paint();
    } else {
        g_rtc.screen = (g_screen == LIBRARY) ? LIBRARY : 0xFF; g_rtc.book = -1; g_rtc.bookId = 0; g_rtc.offset = 0;
        const int w = 380, h = 90;
        g_rtc.rx = (int16_t)((W - w) / 2 - 3); g_rtc.ry = (int16_t)((H - h) / 2 - 3); g_rtc.rw = (int16_t)(w + 6); g_rtc.rh = (int16_t)(h + 6);
        ui::toast("Sleeping", "Press the BOOT button to wake.");
    }
    hwio::deepSleep();
}

static void wake() {
    if (g_rtc.magic != RTC_MAGIC) { display::begin(true); drawLibrary(); return; }
    if (g_rtc.screen == READER && g_rtc.book >= 0 && g_rtc.book < library::count() &&
        library::at(g_rtc.book).id == g_rtc.bookId) {
        display::begin(false);
        // Rebuild exactly what is on the glass, then repair only the footer.
        const library::Book& b = library::at(g_rtc.book);
        const char* txt; uint32_t len;
        if (library::open(g_rtc.book, txt, len) && openText(txt, len, false, b.title, b.author, g_rtc.offset)) {
            g_doc.book = g_rtc.book;
            drawReader(false);
            clearRegion(g_rtc.rx, g_rtc.ry, g_rtc.rw, g_rtc.rh, true);
            paint();
            g_lastTurn = 0;
            return;
        }
        display::hardClear(2);
        drawLibrary();
        return;
    }
    if (g_rtc.screen == LIBRARY) {
        display::begin(false);
        fill(WHITE);
        clearRegion(g_rtc.rx, g_rtc.ry, g_rtc.rw, g_rtc.rh, true);
        drawLibrary();
        return;
    }
    display::begin(true);    // the glass shows a screen we cannot reproduce: scrub it
    drawLibrary();
}

// ---- input ------------------------------------------------------------------------------------------------------------

static void onTap(int x, int y);
static void onSwipe(int dir);       // -1 left (next), +1 right (previous)
static void onLongPress(int x, int y);
static void onBoot();
static void onSide();
static void onBack();               // capacitive Home key: one screen up

static void toLauncher() {
    flushProgress(true);
    settings::save();
    ui::toast("Launcher", "Handing back\xE2\x80\xA6");
    hwio::frontLight(0);
    launcher::returnToLauncher();
}

void begin(bool wakeFromSleep) {
    g_lastInput = millis();
    g_ignoreTouchUntil = millis() + 1500;   // settle time for the touch controller after reset
    if (wakeFromSleep) wake();
    else { display::begin(true); drawLibrary(); }
}

void poll() {
    const uint32_t now = millis();

    // Touch: tap, long press, horizontal swipe.
    static bool down = false, moved = false;
    static int x0 = 0, y0 = 0, xl = 0, yl = 0;
    static uint32_t t0 = 0;
    static int releaseCount = 0;
    static int pressCount = 0;
    int tx, ty;
    const bool pressed = now >= g_ignoreTouchUntil && hwio::touchRead(tx, ty);
    if (pressed) {
        // Two consecutive readings before a gesture starts: a lone stale point is noise.
        if (!down && ++pressCount < 2) { x0 = xl = tx; y0 = yl = ty; }
        else if (!down) { down = true; moved = false; xl = tx; yl = ty; t0 = now; }
        else { xl = tx; yl = ty; if (abs(tx - x0) > 24 || abs(ty - y0) > 24) moved = true; }
        releaseCount = 0;
    } else if (!down) {
        pressCount = 0;
    } else if (down) {
        if (++releaseCount >= 2) {
            down = false;
            g_lastInput = now;
            const int dx = xl - x0, dy = yl - y0;
            if (abs(dx) >= 70 && abs(dx) > abs(dy) * 3 / 2) onSwipe(dx < 0 ? -1 : 1);
            else if (!moved && now - t0 >= 650) onLongPress(x0, y0);
            else if (!moved) onTap(x0, y0);
            g_ignoreTouchUntil = millis() + 150;
        }
    }

    // Buttons.
    static bool bootWas = false, sideWas = false, bootLong = false;
    static uint32_t bootAt = 0, sideCheckAt = 0;
    const bool boot = hwio::bootButtonPressed();
    if (boot && !bootWas) { bootAt = now; bootLong = false; }
    if (boot && !bootLong && now - bootAt >= 1000) { bootLong = true; toLauncher(); }
    if (!boot && bootWas && !bootLong) { g_lastInput = now; onBoot(); }
    bootWas = boot;
    if (now - sideCheckAt >= 25) {
        sideCheckAt = now;
        const bool side = hwio::sideButtonPressed();
        if (side && !sideWas) { g_lastInput = now; onSide(); }
        sideWas = side;
    }

    // Capacitive Home key below the glass. Read here (it piggybacks on the
    // touch poll above) and treated as "back"; every screen also has a tappable
    // way back, so nothing is lost if the key never reports on this board.
    if (hwio::homeKeyPressed()) { g_lastInput = now; onBack(); }

    flushProgress(false);

    const uint32_t sleepMs = settings::sleepMs();
    if (sleepMs && now - g_lastInput > sleepMs && !hwio::usbPowered()) sleepNow();
}

static void jumpToBar(int x) {
    const int bx = ui::MARGIN, bw = W - 2 * ui::MARGIN;
    int pct = (x - bx) * 100 / bw;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    gotoPercent(pct);
}

static void onTap(int x, int y) {
    int arg = 0;
    const int id = ui::hitTest(x, y, &arg);
    switch (g_screen) {
    case READER:
        if (y < READER_MENU_ZONE) drawMenu();
        else if (x < W * 3 / 10) prevPage();
        else nextPage();
        return;
    case HINT:
        settings::get().hints |= 1; settings::save();
        drawReader();
        return;
    case LIBRARY:
        if (id == H_HN_BANNER) { hnEnter(); return; }
        if (id == H_HERO || id == H_BOOK) { if (openBook(arg)) enterReader(); else drawLibrary(); return; }
        if (id == H_LIB_PREV) { g_libPage--; g_libSel = -1; drawLibrary(); return; }
        if (id == H_LIB_NEXT) { g_libPage++; g_libSel = -1; drawLibrary(); return; }
        if (id == H_SETTINGS) { g_settingsFrom = LIBRARY; g_settingsRow = -1; g_settingsPage = 0; drawSettings(); return; }
        return;
    case MENU:
        switch (id) {
            case H_BACK: case H_MENU_CONTINUE: drawReader(); return;
            case H_MENU_CHAPTERS: g_chPage = 0; drawChapters(); return;
            case H_MENU_SETTINGS: g_settingsFrom = MENU; g_settingsRow = -1; g_settingsPage = 0; drawSettings(); return;
            case H_MENU_LIBRARY: if (g_doc.book < 0) { closeDoc(); hnEnter(); } else goToLibrary(); return;
            case H_MENU_BACKTO: { const int p = g_doc.jumpFrom; g_doc.jumpFrom = -1; gotoPage(p + 1); g_doc.jumpFrom = -1; return; }
            case H_MENU_SWITCH: { const int s = g_doc.hnStory; const bool c = g_doc.isComments; if (!hnOpen(s, !c)) drawMenu(); return; }
            case H_MENU_REFRESH: refreshScreen(); return;
            case H_MENU_SLEEP: sleepNow(); return;
            case H_LIGHT_MINUS: if (settings::get().light > 0) settings::get().light--; settingsChanged(false); drawMenu(); return;
            case H_LIGHT_PLUS: if (settings::get().light < 3) settings::get().light++; settingsChanged(false); drawMenu(); return;
            case H_PROGRESS: jumpToBar(x); return;
            default: return;
        }
    case SETTINGS:
        if (id == H_DONE) { leaveSettings(); return; }
        if (id == H_SET_TAB) { g_settingsPage = arg; g_settingsRow = -1; drawSettings(); return; }
        if (id == H_ROW_PREV || id == H_ROW_NEXT) {
            g_settingsRow = arg;
            const bool relayoutNeeded = rowStep(arg, id == H_ROW_NEXT ? 1 : -1);
            settingsChanged(relayoutNeeded);
            drawSettings();
        }
        return;
    case CHAPTERS:
        if (id == H_BACK) { drawMenu(); return; }
        if (id == H_CH_PREV) { g_chPage--; drawChapters(); return; }
        if (id == H_CH_NEXT) { g_chPage++; drawChapters(); return; }
        if (id == H_CHAPTER) {
            const text::Model& m = g_doc.model;
            if (arg >= 0 && (uint32_t)arg < m.chapterCount) {
                const uint32_t pg = layout::pageForOffset(g_doc.pages, m, m.paras[m.chapters[arg]].start);
                gotoPage((int)pg + 1);
            }
        }
        return;
    case END:
        if (id == H_END_LIBRARY) { if (g_doc.book < 0) { closeDoc(); hnEnter(); } else goToLibrary(); return; }
        if (id == H_END_AGAIN) { g_doc.page = 0; g_turnsSinceScrub = 1000; markProgress(); drawReader(); return; }
        return;
    case HN_LIST: case HN_STORY: case WIFI_INFO:
        hnTap(id, arg);
        return;
    }
}

static void onSwipe(int dir) {
    switch (g_screen) {
    case READER: if (dir < 0) nextPage(); else prevPage(); return;
    case LIBRARY: {
        const int pages = gridPages();
        if (dir < 0 && g_libPage + 1 < pages) { g_libPage++; g_libSel = -1; drawLibrary(); }
        else if (dir > 0 && g_libPage > 0) { g_libPage--; g_libSel = -1; drawLibrary(); }
        return;
    }
    case CHAPTERS: {
        const int pages = ((int)g_doc.model.chapterCount + CH_PER_PAGE - 1) / CH_PER_PAGE;
        if (dir < 0 && g_chPage + 1 < pages) { g_chPage++; drawChapters(); }
        else if (dir > 0 && g_chPage > 0) { g_chPage--; drawChapters(); }
        return;
    }
    case HN_LIST: case HN_STORY: hnSwipe(dir); return;
    case MENU: if (dir > 0) drawReader(); return;
    case HINT: onTap(0, 0); return;
    default: return;
    }
}

static void onLongPress(int x, int y) {
    (void)x; (void)y;
    if (g_screen == READER) drawMenu();
    else onTap(x, y);
}

static void onBoot() {
    switch (g_screen) {
    case READER: nextPage(); return;
    case HINT: onTap(0, 0); return;
    case LIBRARY: {
        const int n = library::count();
        const int onPage = n - g_libPage * GRID_PER_PAGE < GRID_PER_PAGE ? n - g_libPage * GRID_PER_PAGE : GRID_PER_PAGE;
        if (onPage <= 0) return;
        g_libSel++;
        if (g_libSel >= onPage) {
            g_libSel = 0;
            if (gridPages() > 1) g_libPage = (g_libPage + 1) % gridPages();
        }
        drawLibrary();
        return;
    }
    case SETTINGS:
        // Walk every row, both pages; drawSettings() follows the row's page.
        if (g_settingsRow < 0) g_settingsRow = g_settingsPage * SETTINGS_PER_PAGE;
        else g_settingsRow = (g_settingsRow + 1) % SETTINGS_ROWS;
        drawSettings();
        return;
    case MENU: drawReader(); return;
    case CHAPTERS: drawMenu(); return;
    case END: onTap(0, 0); if (g_screen == END) { if (g_doc.book < 0) { closeDoc(); hnEnter(); } else goToLibrary(); } return;
    case HN_LIST: case HN_STORY: case WIFI_INFO: hnButton(true); return;
    }
}

static void onSide() {
    switch (g_screen) {
    case READER: prevPage(); return;
    case HINT: onTap(0, 0); return;
    case LIBRARY:
        if (g_libSel >= 0) { const int idx = g_libPage * GRID_PER_PAGE + g_libSel; if (openBook(idx)) enterReader(); }
        else { const int r = library::mostRecent(); if (r >= 0 && openBook(r)) enterReader(); }
        return;
    case SETTINGS:
        if (g_settingsRow >= 0) { const bool rl = rowStep(g_settingsRow, 1); settingsChanged(rl); drawSettings(); }
        return;
    case MENU: drawReader(); return;
    case CHAPTERS: drawMenu(); return;
    case END: if (g_doc.book < 0) { closeDoc(); hnEnter(); } else goToLibrary(); return;
    case HN_LIST: case HN_STORY: case WIFI_INFO: hnButton(false); return;
    }
}

static void onBack() {
    switch (g_screen) {
    case READER: drawMenu(); return;
    case HINT: onTap(0, 0); return;
    case MENU: drawReader(); return;
    case SETTINGS: leaveSettings(); return;
    case CHAPTERS: drawMenu(); return;
    case END: if (g_doc.book < 0) { closeDoc(); hnEnter(); } else goToLibrary(); return;
    case HN_STORY: hnDrawList(); return;
    case HN_LIST: case WIFI_INFO: hnLeave(); goToLibrary(); return;
    case LIBRARY: return;   // already home
    }
}

void showLibrary() { goToLibrary(); }
void openHackerNews() { closeDoc(); hnEnter(); }

void status(Print& out) {
    out.printf("[paperback] screen=%d doc=%s book=%d page=%u/%u chapters=%u glyphs=%u cache=%uKB\n", (int)g_screen,
               g_doc.open ? g_doc.title : "-", g_doc.book, (unsigned)(g_doc.open ? g_doc.page + 1 : 0),
               (unsigned)g_doc.pages.count, (unsigned)g_doc.model.chapterCount, (unsigned)font::cacheGlyphs(),
               (unsigned)(font::cacheBytes() / 1024));
    out.printf("[paperback] heap internal=%u (largest %u) psram=%u  battery=%d%% usb=%d touch=%d sd=%d wifi=%s\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), hwio::batteryPercent(), hwio::usbPowered() ? 1 : 0,
               hwio::touchOk() ? 1 : 0, library::sdMounted() ? 1 : 0, hn::online() ? "online" : (hn::configured() ? "configured" : "unset"));
    const Settings& s = settings::get();
    out.printf("[paperback] settings font=%d size=%d spacing=%d margin=%d justify=%d light=%d fast=%d refresh=%d sleep=%d tz=%s pace=%.1fs/page\n",
               s.fontFamily, s.sizeIdx, s.spacing, s.margin, s.justify, s.light, s.fastTurn, s.refreshIdx, s.sleepIdx,
               settings::tzName(s.tzIdx), s.secPerPage);
}

}  // namespace app
