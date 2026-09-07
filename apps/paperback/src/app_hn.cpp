// Hacker News screens: front page list, story card, Wi-Fi help.
#include "app.h"
#include "app_internal.h"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font.h"
#include "hn.h"
#include "hwio.h"
#include "launcher_api.h"
#include "settings.h"
#include "ui.h"

using namespace display;

namespace app {

static int  g_hnPage = 0;
static int  g_hnSel = -1;
static int  g_hnStory = -1;
static char g_hnErr[96] = {0};
static const int HN_PER_PAGE = 7;   // 7 x 100 px rows from 128 end at 828, above the bar

static bool ensureOnline(bool showToast) {
    if (hn::online()) return true;
    if (!hn::configured()) return false;
    if (showToast) {
        char l2[80]; snprintf(l2, sizeof l2, "Joining %s", settings::get().wifiSsid);
        ui::toast("Connecting\xE2\x80\xA6", l2);
    }
    if (!hn::connect(20000)) { snprintf(g_hnErr, sizeof g_hnErr, "Could not join %s", settings::get().wifiSsid); return false; }
    if (!hn::haveTime()) hn::syncClock();
    return true;
}

void hnDrawWifiInfo() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Hacker News", "Library", H_BACK);
    ui::hnLogo(ui::MARGIN, 100, 56);
    font::draw(ui::MARGIN + 74, 138, "Wi-Fi needed", font::SERIF_BOLD, 28, BLACK);
    int y = 210;
    if (g_hnErr[0]) { font::drawWrapped(ui::MARGIN, y, ui::CONTENT_W, g_hnErr, font::SANS_BOLD, 17, 22, 2, BLACK, 0); y += 60; }
    font::drawWrapped(ui::MARGIN, y, ui::CONTENT_W,
                      "Paperback needs a network to fetch stories. Give it one in either of two ways:",
                      font::SANS, 17, 23, 3, BLACK, 0);
    y += 90;
    font::draw(ui::MARGIN, y, "1", font::SERIF_BOLD, 30, BLACK);
    font::drawWrapped(ui::MARGIN + 40, y - 6, ui::CONTENT_W - 40,
                      "Put a file named wifi.txt in a folder called paperback on the SD card. First line: network name. Second line: password. Restart the app.",
                      font::SANS, 16, 22, 4, BLACK, 0);
    y += 120;
    font::draw(ui::MARGIN, y, "2", font::SERIF_BOLD, 30, BLACK);
    font::drawWrapped(ui::MARGIN + 40, y - 6, ui::CONTENT_W - 40,
                      "Over USB, in the serial console (tools/flash.py cmd), type:",
                      font::SANS, 16, 22, 2, BLACK, 0);
    fillRoundRect(ui::MARGIN + 40, y + 50, ui::CONTENT_W - 40, 44, 6, LIGHT);
    font::draw(ui::MARGIN + 54, y + 80, "wifi \"Network name\" \"password\"", font::SANS_BOLD, 17, BLACK);
    y += 130;
    font::drawWrapped(ui::MARGIN, y, ui::CONTENT_W,
                      "Articles are fetched as plain text through the r.jina.ai reader service; comments come from the Hacker News API.",
                      font::SANS, 14, 19, 3, DARK, 0);
    // Text ends by 720 at the latest; one full-width 96 px button at 840..936.
    ui::button(ui::MARGIN, 840, ui::CONTENT_W, 96, hn::configured() ? "Try again" : "Back to library", true,
               hn::configured() ? H_WIFI_RETRY : H_BACK);
    g_screen = WIFI_INFO;
    paint();
}

static void refreshFront() {
    ui::toast("Loading the front page\xE2\x80\xA6");
    char err[80];
    if (hn::refreshFront(err, sizeof err) < 0) snprintf(g_hnErr, sizeof g_hnErr, "Could not load stories: %s", err);
    else g_hnErr[0] = 0;
    g_hnPage = 0; g_hnSel = -1;
}

void hnEnter() {
    g_hnErr[0] = 0;
    if (!hn::configured()) { hnDrawWifiInfo(); return; }
    if (!ensureOnline(true)) { hnDrawWifiInfo(); return; }
    if (hn::frontCount() == 0 || hn::frontAgeMs() > 20UL * 60UL * 1000UL) refreshFront();
    hnDrawList();
}

void hnLeave() {
    hn::disconnect();
}

void hnDrawList() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Hacker News", "Library", H_BACK);
    const int n = hn::frontCount();
    const int pages = n ? (n + HN_PER_PAGE - 1) / HN_PER_PAGE : 1;
    if (g_hnPage >= pages) g_hnPage = pages - 1;
    if (g_hnPage < 0) g_hnPage = 0;

    // Sub header at 118, rows 128..828, one small line at 846, bar 856..944.
    char sub[64];
    const uint32_t age = hn::frontAgeMs();
    if (n && age != UINT32_MAX) {
        if (age < 60000) snprintf(sub, sizeof sub, "Front page  \xC2\xB7  just now");
        else snprintf(sub, sizeof sub, "Front page  \xC2\xB7  %lu min ago", (unsigned long)(age / 60000));
    } else snprintf(sub, sizeof sub, "Front page");
    ui::sectionLabel(ui::MARGIN, 118, sub);

    if (n == 0) {
        font::drawWrapped(ui::MARGIN, 200, ui::CONTENT_W, g_hnErr[0] ? g_hnErr : "Nothing loaded yet.", font::SANS, 17, 23, 4, BLACK, 0);
        font::drawWrapped(ui::MARGIN, 300, ui::CONTENT_W, "Tap Refresh below to try again.", font::SANS, 15, 20, 1, DARK, 0);
    }

    const int rowH = 100;
    int y = 128;
    for (int i = g_hnPage * HN_PER_PAGE; i < n && i < (g_hnPage + 1) * HN_PER_PAGE; ++i, y += rowH) {
        const hn::Story& s = hn::story(i);
        const int local = i - g_hnPage * HN_PER_PAGE;
        if (g_hnSel == local) fillRoundRect(ui::MARGIN - 10, y + 2, ui::CONTENT_W + 20, rowH - 6, 8, LIGHT);
        char rank[8]; snprintf(rank, sizeof rank, "%d", i + 1);
        font::drawRight(ui::MARGIN + 26, y + 36, rank, font::SANS_BOLD, 16, DARK);
        const int tx = ui::MARGIN + 40, tw = W - ui::MARGIN - tx;
        const int lines = font::drawWrapped(tx, y + 36, tw, s.title, font::SERIF, 20, 25, 2, BLACK, 0);
        char meta[120], when[16];
        hn::ago(s.createdAt, when, sizeof when);
        size_t k = 0;
        if (s.points >= 0) k += snprintf(meta + k, sizeof meta - k, "%d points", s.points);
        if (s.comments >= 0 && k < sizeof meta) k += snprintf(meta + k, sizeof meta - k, "%s%d comments", k ? "  \xC2\xB7  " : "", s.comments);
        if (s.domain[0] && k < sizeof meta) k += snprintf(meta + k, sizeof meta - k, "%s%s", k ? "  \xC2\xB7  " : "", s.domain);
        if (when[0] && k < sizeof meta) snprintf(meta + k, sizeof meta - k, "%s%s ago", k ? "  \xC2\xB7  " : "", when);
        font::drawFit(tx, y + 36 + lines * 25 + 4, meta, font::SANS, 13, tw, DARK);
        hline(ui::MARGIN, y + rowH - 2, ui::CONTENT_W, LIGHT);
        ui::hit(0, y, W, rowH, H_HN_STORY, i);
    }

    font::draw(ui::MARGIN, 846, "BOOT: next  \xC2\xB7  side: open", font::SANS, 13, DARK);
    if (pages > 1) {
        char pg[24]; snprintf(pg, sizeof pg, "Page %d of %d", g_hnPage + 1, pages);
        font::drawRight(W - ui::MARGIN, 846, pg, font::SANS, 13, DARK);
    }
    // Bar: [Refresh 292] [<] [>] = 292 + 88 + 88 with 12 px gutters.
    const int pw = ui::CONTENT_W - 2 * (ui::TOUCH_MIN + ui::GUTTER);   // 292
    ui::button(ui::MARGIN, ui::BAR_Y, pw, ui::BAR_H, "Refresh", false, H_HN_REFRESH);
    ui::chevronButton(ui::MARGIN + pw + ui::GUTTER, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, false, H_HN_PREV, 0, g_hnPage > 0);
    ui::chevronButton(W - ui::MARGIN - ui::TOUCH_MIN, ui::BAR_Y, ui::TOUCH_MIN, ui::TOUCH_MIN, true, H_HN_NEXT, 0, g_hnPage + 1 < pages);
    g_screen = HN_LIST;
    paint();
}

void hnDrawStory() {
    fill(WHITE);
    ui::hitsClear();
    ui::header("Story", "Front page", H_BACK);
    if (g_hnStory < 0 || g_hnStory >= hn::frontCount()) { hnDrawList(); return; }
    const hn::Story& s = hn::story(g_hnStory);
    char rank[16]; snprintf(rank, sizeof rank, "#%d on the front page", g_hnStory + 1);
    ui::sectionLabel(ui::MARGIN, 108, rank);
    const int lines = font::drawWrapped(ui::MARGIN, 152, ui::CONTENT_W, s.title, font::SERIF_BOLD, 30, 37, 4, BLACK, 0);
    int y = 152 + lines * 37 + 6;
    if (s.domain[0]) { font::drawFit(ui::MARGIN, y, s.domain, font::SANS_BOLD, 17, ui::CONTENT_W, DARK); y += 30; }
    char meta[160], when[16];
    hn::ago(s.createdAt, when, sizeof when);
    snprintf(meta, sizeof meta, "%d points  \xC2\xB7  %d comments  \xC2\xB7  by %s%s%s%s", s.points < 0 ? 0 : s.points,
             s.comments < 0 ? 0 : s.comments, s.author, when[0] ? "  \xC2\xB7  " : "", when, when[0] ? " ago" : "");
    font::drawWrapped(ui::MARGIN, y, ui::CONTENT_W, meta, font::SANS, 15, 20, 2, DARK, 0);
    y += 70;
    hline(ui::MARGIN, y, ui::CONTENT_W, LIGHT);
    y += 40;
    // Two full-width 88 px actions; with a four-line title they sit at 446 and
    // 546, the URL and any error below them end by 790.
    const bool hasUrl = s.url[0] != 0;
    ui::button(ui::MARGIN, y, ui::CONTENT_W, ui::TOUCH_MIN, hasUrl ? "Read the article" : "Read the post", true, H_HN_ARTICLE);
    y += ui::TOUCH_MIN + ui::GUTTER;
    ui::button(ui::MARGIN, y, ui::CONTENT_W, ui::TOUCH_MIN, "Read the comments", false, H_HN_COMMENTS);
    y += ui::TOUCH_MIN + 24;
    if (hasUrl) font::drawWrapped(ui::MARGIN, y, ui::CONTENT_W, s.url, font::SANS, 13, 17, 3, DARK, 0);
    if (g_hnErr[0]) font::drawWrapped(ui::MARGIN, y + 70, ui::CONTENT_W, g_hnErr, font::SANS_BOLD, 15, 20, 3, BLACK, 0);
    font::draw(ui::MARGIN, H - 36, "BOOT: article  \xC2\xB7  side: comments", font::SANS, 13, DARK);
    g_screen = HN_STORY;
    paint();
}

bool hnOpen(int storyIdx, bool comments) {
    if (storyIdx < 0 || storyIdx >= hn::frontCount()) return false;
    const hn::Story s = hn::story(storyIdx);
    if (!ensureOnline(true)) { hnDrawWifiInfo(); return false; }
    if (!comments && !s.url[0]) comments = true;          // Ask HN: the post is the thread
    ui::toast(comments ? "Loading the comments\xE2\x80\xA6" : "Fetching the article\xE2\x80\xA6", comments ? s.title : s.domain);
    char err[80];
    uint32_t len = 0;
    char* txt = comments ? hn::fetchComments(s, len, err, sizeof err) : hn::fetchArticle(s, len, err, sizeof err);
    if (!txt) {
        snprintf(g_hnErr, sizeof g_hnErr, "%s: %s", comments ? "Comments failed" : "Article failed", err);
        g_hnStory = storyIdx;
        hnDrawStory();
        return false;
    }
    g_hnErr[0] = 0;
    closeDoc();
    char sub[120];
    snprintf(sub, sizeof sub, "%s%s%s", comments ? "Comments" : (s.domain[0] ? s.domain : "Hacker News"), s.author[0] ? "  \xC2\xB7  by " : "", s.author);
    if (!openText(txt, len, true, s.title, sub, 0)) { g_hnStory = storyIdx; hnDrawStory(); return false; }
    g_doc.book = -1;
    g_doc.hnStory = storyIdx;
    g_doc.isComments = comments;
    enterReader();
    return true;
}

bool hnTap(int id, int arg) {
    switch (g_screen) {
    case WIFI_INFO:
        if (id == H_BACK) { hnLeave(); goToLibrary(); return true; }
        if (id == H_WIFI_RETRY) { hnEnter(); return true; }
        return false;
    case HN_LIST:
        if (id == H_BACK) { hnLeave(); goToLibrary(); return true; }
        if (id == H_HN_REFRESH) { if (ensureOnline(true)) refreshFront(); hnDrawList(); return true; }
        if (id == H_HN_PREV) { g_hnPage--; g_hnSel = -1; hnDrawList(); return true; }
        if (id == H_HN_NEXT) { g_hnPage++; g_hnSel = -1; hnDrawList(); return true; }
        if (id == H_HN_STORY) { g_hnStory = arg; g_hnErr[0] = 0; hnDrawStory(); return true; }
        return false;
    case HN_STORY:
        if (id == H_BACK) { hnDrawList(); return true; }
        if (id == H_HN_ARTICLE) { hnOpen(g_hnStory, false); return true; }
        if (id == H_HN_COMMENTS) { hnOpen(g_hnStory, true); return true; }
        return false;
    default: return false;
    }
}

void hnButton(bool boot) {
    switch (g_screen) {
    case HN_LIST: {
        const int n = hn::frontCount();
        const int onPage = n - g_hnPage * HN_PER_PAGE < HN_PER_PAGE ? n - g_hnPage * HN_PER_PAGE : HN_PER_PAGE;
        if (boot) {
            if (onPage <= 0) return;
            g_hnSel++;
            if (g_hnSel >= onPage) { g_hnSel = 0; const int pages = (n + HN_PER_PAGE - 1) / HN_PER_PAGE; g_hnPage = (g_hnPage + 1) % pages; }
            hnDrawList();
        } else if (g_hnSel >= 0) {
            g_hnStory = g_hnPage * HN_PER_PAGE + g_hnSel; g_hnErr[0] = 0; hnDrawStory();
        }
        return;
    }
    case HN_STORY:
        hnOpen(g_hnStory, !boot);
        return;
    case WIFI_INFO:
        if (boot) hnEnter(); else { hnLeave(); goToLibrary(); }
        return;
    default: return;
    }
}

void hnSwipe(int dir) {
    if (g_screen == HN_LIST) {
        const int n = hn::frontCount();
        const int pages = n ? (n + HN_PER_PAGE - 1) / HN_PER_PAGE : 1;
        if (dir < 0 && g_hnPage + 1 < pages) { g_hnPage++; g_hnSel = -1; hnDrawList(); }
        else if (dir > 0 && g_hnPage > 0) { g_hnPage--; g_hnSel = -1; hnDrawList(); }
    } else if (g_screen == HN_STORY && dir > 0) {
        hnDrawList();
    }
}

}  // namespace app
