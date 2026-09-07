#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_api.h"

namespace settings {

static Settings g_s;
static Settings g_saved;
static const uint8_t VERSION = 1;

static const float kSizes[SIZE_COUNT] = {19.f, 22.f, 25.f, 28.f, 32.f, 36.f, 41.f};
static const float kSpacing[3] = {1.25f, 1.42f, 1.62f};
static const int   kMargins[3] = {18, 32, 48};
// Same steps as the launcher and OpenTrailPaper, so the shared duty maps cleanly.
static const uint8_t kLightDuty[4] = {0, 12, 90, 230};

struct Tz { const char* name; const char* posix; };
static const Tz kTz[TZ_COUNT] = {
    {"UTC",                    "UTC0"},
    {"Central Europe",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UK / Ireland",           "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe",         "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow / Istanbul",      "MSK-3"},
    {"India",                  "IST-5:30"},
    {"China / Singapore",      "CST-8"},
    {"Japan / Korea",          "JST-9"},
    {"Australia East",         "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"US Eastern",             "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central",             "CST6CDT,M3.2.0,M11.1.0"},
    {"US Pacific",             "PST8PDT,M3.2.0,M11.1.0"},
};

static void defaults(Settings& s) {
    memset(&s, 0, sizeof s);
    s.version = VERSION;
    s.fontFamily = 0;
    s.sizeIdx = 2;
    s.spacing = 1;
    s.margin = 1;
    s.justify = 1;
    s.light = 0;
    s.fastTurn = 0;
    s.refreshIdx = 2;
    s.sleepIdx = 1;
    s.tzIdx = 1;
    s.hints = 0;
    s.secPerPage = 0.f;
}

Settings& get() { return g_s; }

void load() {
    defaults(g_s);
    Preferences p;
    if (p.begin("paperback", true)) {
        Settings tmp;
        const size_t n = p.getBytes("cfg", &tmp, sizeof tmp);
        p.end();
        if (n == sizeof tmp && tmp.version == VERSION) g_s = tmp;
        else if (n > 0) Serial.println("[settings] stored config is from another version; using defaults");
    }
    // Clamp whatever came back.
    if (g_s.sizeIdx >= SIZE_COUNT) g_s.sizeIdx = 2;
    if (g_s.spacing > 2) g_s.spacing = 1;
    if (g_s.margin > 2) g_s.margin = 1;
    if (g_s.light > 3) g_s.light = 0;
    // The launcher and other apps write the shared duty; follow whoever set it last.
    if (launcher::hasFrontLight()) g_s.light = lightLevelFromDuty(launcher::frontLight());
    if (g_s.refreshIdx > 3) g_s.refreshIdx = 2;
    if (g_s.sleepIdx > 3) g_s.sleepIdx = 1;
    if (g_s.tzIdx >= TZ_COUNT) g_s.tzIdx = 1;
    g_s.wifiSsid[sizeof g_s.wifiSsid - 1] = 0;
    g_s.wifiPass[sizeof g_s.wifiPass - 1] = 0;
    if (!(g_s.secPerPage >= 0.f && g_s.secPerPage < 3600.f)) g_s.secPerPage = 0.f;
    g_saved = g_s;
}

void save() {
    if (memcmp(&g_s, &g_saved, sizeof g_s) == 0) return;
    Preferences p;
    if (p.begin("paperback", false)) {
        p.putBytes("cfg", &g_s, sizeof g_s);
        p.end();
        g_saved = g_s;
    }
}

float bodyPx() { return kSizes[g_s.sizeIdx]; }
int lineHeight() { return (int)lroundf(bodyPx() * kSpacing[g_s.spacing]); }
int marginPx() { return kMargins[g_s.margin]; }
uint32_t sleepMs() {
    static const uint32_t m[4] = {5, 10, 30, 0};
    return m[g_s.sleepIdx] * 60000UL;
}
int refreshEvery() {
    static const int r[4] = {0, 5, 10, 20};
    return r[g_s.refreshIdx];
}
uint8_t lightDuty() { return kLightDuty[g_s.light > 3 ? 3 : g_s.light]; }
uint8_t lightLevelFromDuty(uint8_t duty) {
    int best = 0;
    for (int i = 1; i < 4; ++i) if (abs((int)duty - (int)kLightDuty[i]) < abs((int)duty - (int)kLightDuty[best])) best = i;
    return (uint8_t)best;
}
const char* lightName(int level) {
    static const char* names[4] = {"Off", "Low", "Medium", "High"};
    return names[level < 0 ? 0 : (level > 3 ? 3 : level)];
}
const char* tzName(int idx) { return kTz[idx < 0 || idx >= TZ_COUNT ? 0 : idx].name; }
const char* tzPosix(int idx) { return kTz[idx < 0 || idx >= TZ_COUNT ? 0 : idx].posix; }

}  // namespace settings
