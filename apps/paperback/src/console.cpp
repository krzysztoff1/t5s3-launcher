#include "console.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_internal.h"
#include "display.h"
#include "font.h"
#include "hn.h"
#include "hwio.h"
#include "launcher_api.h"
#include "library.h"
#include "settings.h"

namespace console {

static char g_line[256];
static size_t g_len = 0;

static int tokenize(char* s, char** argv, int max) {
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            s++;
            argv[argc++] = s;
            while (*s && *s != '"') s++;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    return argc;
}

static void help() {
    Serial.println("[paperback] commands:");
    Serial.println("  help                         this list");
    Serial.println("  list                         books with progress");
    Serial.println("  open <n>                     open book n (from list)");
    Serial.println("  next | prev                  turn the page");
    Serial.println("  page <n> | goto <percent>    jump");
    Serial.println("  menu | library | hn          screens");
    Serial.println("  refresh                      full panel refresh");
    Serial.println("  light <0-3>                  front light: off, low, medium, high (shared with the launcher)");
    Serial.println("  size <0-6> | font serif|sans text settings");
    Serial.println("  wifi \"<ssid>\" \"<password>\"   store Wi-Fi credentials (wifi clear / wifi status)");
    Serial.println("  time                         sync the clock over NTP (connects to Wi-Fi)");
    Serial.println("  gamma <t3> <t2> <t1>         glyph coverage thresholds (default 140 75 28)");
    Serial.println("  status                       memory, document, settings");
    Serial.println("  screenshot                   dump the framebuffer (tools/flash.py screenshot writes a PNG)");
    Serial.println("  sleep                        deep sleep now (BOOT wakes)");
    Serial.println("  launcher                     hand back to the launcher menu");
    Serial.println("  bootloader                   reboot into USB download mode");
    Serial.println("  reboot | ver");
}

static void list() {
    for (int i = 0; i < library::count(); ++i) {
        const library::Book& b = library::at(i);
        Serial.printf("[paperback] %2d  %-40.40s  %-24.24s  %7u B  %3d%%%s%s\n", i, b.title, b.author, (unsigned)b.size,
                      b.percent, b.finished ? " finished" : "", b.src == library::SDCARD ? "  (SD)" : "");
    }
}

static void handle(char* line) {
    char* argv[6];
    const int argc = tokenize(line, argv, 6);
    if (argc == 0) return;
    const char* c = argv[0];
    if (!strcasecmp(c, "help") || !strcmp(c, "?")) help();
    else if (!strcasecmp(c, "list")) list();
    else if (!strcasecmp(c, "open") && argc > 1) {
        const int n = atoi(argv[1]);
        if (app::openBook(n)) { Serial.printf("[paperback] opened %s\n", library::at(n).title); app::enterReader(); }
        else Serial.println("[paperback] no such book");
    }
    else if (!strcasecmp(c, "next")) { if (!app::nextPage()) Serial.println("[paperback] no book open"); }
    else if (!strcasecmp(c, "prev")) { if (!app::prevPage()) Serial.println("[paperback] at the first page or no book open"); }
    else if (!strcasecmp(c, "page") && argc > 1) { if (!app::gotoPage(atoi(argv[1]))) Serial.println("[paperback] out of range"); }
    else if (!strcasecmp(c, "goto") && argc > 1) { if (!app::gotoPercent(atoi(argv[1]))) Serial.println("[paperback] no book open"); }
    else if (!strcasecmp(c, "menu")) app::showMenu();
    else if (!strcasecmp(c, "library")) app::showLibrary();
    else if (!strcasecmp(c, "hn")) app::openHackerNews();
    else if (!strcasecmp(c, "refresh")) app::refreshScreen();
    else if (!strcasecmp(c, "light") && argc > 1) { int v = atoi(argv[1]); if (v < 0) v = 0; if (v > 3) v = 3; settings::get().light = (uint8_t)v; app::settingsChanged(false); Serial.printf("[paperback] light %s\n", settings::lightName(v)); }
    else if (!strcasecmp(c, "size") && argc > 1) { int v = atoi(argv[1]); if (v < 0) v = 0; if (v >= settings::SIZE_COUNT) v = settings::SIZE_COUNT - 1; settings::get().sizeIdx = (uint8_t)v; app::settingsChanged(true); app::refreshScreen(); }
    else if (!strcasecmp(c, "font") && argc > 1) { settings::get().fontFamily = !strcasecmp(argv[1], "sans") ? 1 : 0; app::settingsChanged(true); app::refreshScreen(); }
    else if (!strcasecmp(c, "wifi")) {
        Settings& s = settings::get();
        if (argc >= 2 && !strcasecmp(argv[1], "clear")) { s.wifiSsid[0] = 0; s.wifiPass[0] = 0; settings::save(); Serial.println("[paperback] wifi cleared"); }
        else if (argc >= 3) { strlcpy(s.wifiSsid, argv[1], sizeof s.wifiSsid); strlcpy(s.wifiPass, argv[2], sizeof s.wifiPass); settings::save(); Serial.printf("[paperback] wifi stored for \"%s\"\n", s.wifiSsid); }
        else Serial.printf("[paperback] wifi %s%s (%s)\n", s.wifiSsid[0] ? "configured: " : "not configured", s.wifiSsid, hn::online() ? "online" : "offline");
    }
    else if (!strcasecmp(c, "time")) {
        if (!hn::configured()) Serial.println("[paperback] set wifi first");
        else if (hn::connect(20000) && hn::syncClock()) { int h, m; if (hwio::clock(h, m)) Serial.printf("[paperback] clock %02d:%02d\n", h, m); hn::disconnect(); }
        else Serial.println("[paperback] time sync failed");
    }
    else if (!strcasecmp(c, "gamma") && argc > 3) { font::setThresholds((uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3])); app::refreshScreen(); }
    else if (!strcasecmp(c, "status") || !strcasecmp(c, "stats")) app::status(Serial);
    else if (!strcasecmp(c, "screenshot") || !strcasecmp(c, "shot")) {
        if (!display::ready()) Serial.println("[screenshot] the display is not up");
        else launcher::dumpScreen(display::fb(), display::W, display::H);
    }
    else if (!strcasecmp(c, "sleep")) app::sleepNow();
    else if (!strcasecmp(c, "launcher")) { Serial.println("[paperback] returning to the launcher"); delay(20); launcher::returnToLauncher(); }
    else if (!strcasecmp(c, "bootloader")) hwio::rebootToDownloadMode();
    else if (!strcasecmp(c, "reboot")) { delay(20); esp_restart(); }
    else if (!strcasecmp(c, "ver")) Serial.printf("[paperback] %s %s\n", APP_NAME, APP_VERSION);
    else Serial.printf("[paperback] unknown command \"%s\" (try help)\n", c);
}

void poll() {
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (g_len) { g_line[g_len] = 0; handle(g_line); g_len = 0; }
        } else if (g_len < sizeof g_line - 1) {
            g_line[g_len++] = ch;
        }
    }
}

}  // namespace console
