#include "console.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include <SD.h>
#include <esp_heap_caps.h>

#include "appstore.h"
#include "display.h"
#include "hw.h"
#include "launcher_api.h"
#include "registry.h"
#include "sdcard.h"
#include "syscheck.h"
#include "ui.h"
#include "version.h"

namespace console {

static char g_line[192];
constexpr size_t PUT_CHUNK = 4096;
static size_t g_len = 0;
static bool g_saw = false;

bool sawInput() { return g_saw; }

// Split on spaces, honouring "double quotes". Returns argc.
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
    Serial.println("[launcher] commands:");
    Serial.println("  help                          this list");
    Serial.println("  list                          app slots: validity, name, version");
    Serial.println("  boot <ota_0|ota_1>            start an app now");
    Serial.println("  apps                          apps in the SD store (/apps/<id>/): installed where, size, problems");
    Serial.println("  install <id> [slot] [confirm] [stay]   copy an SD app into a slot and start it");
    Serial.println("                                (ota_0, or a slot whose app is not on the card, needs `confirm`;");
    Serial.println("                                 `stay` installs without starting)");
    Serial.println("  put <path> <bytes>            receive a file onto the SD card (tools/flash.py put/sdapp)");
    Serial.println("  ls [dir]  /  rm <path>        SD card files (rm also takes an app directory)");
    Serial.println("  register <slot> \"<name>\" \"<version>\"   label a slot for the menu");
    Serial.println("  unregister <slot>             drop a slot's label");
    Serial.println("  menu [home|settings|sd]       show a menu screen (brings the display up)");
    Serial.println("                                on screen: Settings holds touch flip, light, system check, sleep, flash mode");
    Serial.println("  syscheck [quick]              run the hardware check (quick = no touch/button steps)");
    Serial.println("  touchflip on|off              rotate touch input 180 degrees (persisted)");
    Serial.println("  nvs                           dump the launcher's NVS state");
    Serial.println("  forget                        wipe the launcher's NVS state");
    Serial.println("  light <0-255>                 front light PWM duty, shared with every app");
    Serial.println("  sleep                         deep sleep (BOOT wakes)");
    Serial.println("  bootloader                    reboot into USB download mode");
    Serial.println("  reboot                        restart");
    Serial.println("  screenshot                    dump the framebuffer (tools/flash.py screenshot writes a PNG)");
    Serial.println("  ver                           launcher version");
}

static void list() {
    registry::Slot slots[8];
    const int n = registry::scan(slots, 8);
    for (int i = 0; i < n; ++i) {
        const registry::Slot& s = slots[i];
        Serial.printf("[launcher] slot %s offset=0x%06X size=%u valid=%d name=\"%s\" version=\"%s\"%s%s\n",
                      registry::slotLabel(s.index), (unsigned)s.part->address, (unsigned)s.part->size,
                      s.valid ? 1 : 0, s.name, s.version, s.running ? " running" : "", s.last ? " last" : "");
    }
    Serial.printf("[launcher] last=%d (no autostart; the menu always shows on boot)\n", registry::lastSlot());
}

// --- SD card -----------------------------------------------------------------

static void listApps() {
    static appstore::App apps[appstore::MAX_APPS];   // ~5 KB: not on the loop stack
    if (!appstore::cardReady()) { Serial.println("[apps] no SD card"); return; }
    const int n = appstore::scan(apps, appstore::MAX_APPS);
    for (int i = 0; i < n; ++i) {
        const appstore::App& a = apps[i];
        char where[24];
        if (a.installedIn >= 0) snprintf(where, sizeof where, "installed=%s", registry::slotLabel(a.installedIn));
        else snprintf(where, sizeof where, "installed=no");
        Serial.printf("[apps] %s name=\"%s\" version=\"%s\" size=%u slot=%s %s%s%s\n", a.id, a.name, a.version,
                      (unsigned)a.size, registry::slotLabel(a.slot), where,
                      a.problem[0] ? " problem=" : "", a.problem);
    }
    Serial.printf("[apps] %d app%s in /apps\n", n, n == 1 ? "" : "s");
}

static void install(int argc, char** argv) {
    if (argc < 2) { Serial.println("[install] install <id> [ota_0|ota_1] [confirm] [stay]"); return; }
    int slot = -2;
    bool confirm = false, stay = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcasecmp(argv[i], "confirm")) confirm = true;
        else if (!strcasecmp(argv[i], "stay")) stay = true;
        else if ((slot = registry::slotFromLabel(argv[i])) < 0 || !registry::otaPartition(slot)) {
            Serial.printf("[install] \"%s\" is not an OTA slot\n", argv[i]);
            return;
        }
    }
    static appstore::App app;
    if (!appstore::find(argv[1], app)) { Serial.printf("[install] no app \"%s\" in /apps (try `apps`)\n", argv[1]); return; }
    if (app.problem[0]) { Serial.printf("[install] %s: %s\n", app.id, app.problem); return; }
    const appstore::Plan plan = appstore::plan(app, slot);
    if (plan.needsConfirm && !confirm && !plan.already) {
        Serial.printf("[install] %s would replace %s in %s%s; add `confirm` to go ahead\n", app.name,
                      plan.replaces[0] ? plan.replaces : "the app", registry::slotLabel(plan.slot),
                      plan.lossy ? ", which is NOT on the SD card" : "");
        return;
    }
    ui::installAndOpen(app, plan, !stay);
}

// `put <path> <byte-count>`, then the raw bytes. OpenTrailPaper's protocol:
// USB-CDC drops what does not fit its RX queue, so the host sends PUT_CHUNK
// bytes and waits for "[put] ack" before the next chunk. The launcher's queue
// is enlarged in setup(), so its chunks are bigger than OpenTrailPaper's.
static void put(int argc, char** argv) {
    const long total = argc > 2 ? atol(argv[2]) : 0;
    if (argc < 3 || argv[1][0] != '/' || total <= 0) {
        Serial.println("[put] put </path/on/sd> <byte-count>, then send raw bytes");
        return;
    }
    if (!sdcard::mount()) { Serial.println("[put] no SD card"); return; }
    char path[96];
    strlcpy(path, argv[1], sizeof path);
    sdcard::mkdirsFor(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f && sdcard::remount()) { sdcard::mkdirsFor(path); f = SD.open(path, FILE_WRITE); }
    if (!f) { Serial.printf("[put] cannot open %s\n", path); return; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(PUT_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); Serial.println("[put] out of memory"); return; }
    Serial.printf("[put] send %ld raw bytes now, %u per ack\n", total, (unsigned)PUT_CHUNK);
    long left = total;
    bool ok = true;
    const char* why = "";
    while (left > 0 && ok) {
        const size_t want = min((long)PUT_CHUNK, left);
        size_t got = 0;
        uint32_t last = millis();
        while (got < want) {
            const size_t n = Serial.read(buf + got, want - got);
            if (n) { got += n; last = millis(); continue; }
            if (millis() - last > 5000) { ok = false; why = " (timed out)"; break; }
            delay(1);
        }
        if (got && f.write(buf, got) != got) { ok = false; why = " (SD write failed)"; }
        left -= got;
        if (ok && left > 0) Serial.println("[put] ack");
    }
    f.close();
    heap_caps_free(buf);
    Serial.printf("[put] %s: %ld bytes written%s\n", ok ? "done" : "FAILED", total - left, why);
    if (!strncmp(path, "/apps/", 6)) ui::draw();   // a new or updated SD app shows on HOME
}

static void ls(const char* dir) {
    if (!sdcard::mount()) { Serial.println("[ls] no SD card"); return; }
    File d = SD.open(dir ? dir : "/");
    if (!d && sdcard::remount()) d = SD.open(dir ? dir : "/");
    if (!d || !d.isDirectory()) { Serial.printf("[ls] no directory %s\n", dir ? dir : "/"); return; }
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        if (e.isDirectory()) Serial.printf("[ls] %s/\n", e.name());
        else Serial.printf("[ls] %s %u\n", e.name(), (unsigned)e.size());
        e.close();
    }
    d.close();
}

// A file, or a directory one level deep (an app's /apps/<id>).
static void rm(const char* path) {
    if (!path || path[0] != '/' || !strcmp(path, "/")) { Serial.println("[rm] rm </path/on/sd>"); return; }
    if (!sdcard::mount()) { Serial.println("[rm] no SD card"); return; }
    File d = SD.open(path);
    if (!d) { Serial.printf("[rm] no %s\n", path); return; }
    bool ok;
    if (d.isDirectory()) {
        char child[128];
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            snprintf(child, sizeof child, "%s/%s", path, e.name());
            const bool sub = e.isDirectory();
            e.close();
            if (!sub) SD.remove(child);
        }
        d.close();
        ok = SD.rmdir(path);
    } else {
        d.close();
        ok = SD.remove(path);
    }
    Serial.printf("[rm] %s %s\n", path, ok ? "removed" : "NOT removed (a directory with directories in it?)");
    if (!strncmp(path, "/apps/", 6)) ui::draw();
}

static bool onoff(const char* a, bool& out) {
    if (!a) return false;
    if (!strcasecmp(a, "on") || !strcmp(a, "1")) { out = true; return true; }
    if (!strcasecmp(a, "off") || !strcmp(a, "0")) { out = false; return true; }
    return false;
}

static void handle(char* line) {
    char* argv[6];
    const int argc = tokenize(line, argv, 6);
    if (argc == 0) return;
    if (!strcasecmp(argv[0], "light")) {
        if (argc > 1) {
            int d = atoi(argv[1]); if (d < 0) d = 0; if (d > 255) d = 255;
            launcher::setFrontLight((uint8_t)d);
            hw::backlight((uint8_t)d);
        }
        Serial.printf("[launcher] front light duty %u\n", (unsigned)launcher::frontLight());
        ui::draw();
        return;
    }
    const char* cmd = argv[0];
    const char* a1 = argc > 1 ? argv[1] : nullptr;

    if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
        help();
    } else if (!strcasecmp(cmd, "list")) {
        list();
    } else if (!strcasecmp(cmd, "boot")) {
        const int idx = registry::slotFromLabel(a1);
        if (idx < 0) { Serial.println("[launcher] boot <ota_0|ota_1>"); return; }
        if (!registry::boot(idx, esp_reset_reason()))
            Serial.println("[launcher] boot failed");
    } else if (!strcasecmp(cmd, "apps")) {
        listApps();
    } else if (!strcasecmp(cmd, "install")) {
        install(argc, argv);
    } else if (!strcasecmp(cmd, "put")) {
        put(argc, argv);
    } else if (!strcasecmp(cmd, "ls")) {
        ls(a1);
    } else if (!strcasecmp(cmd, "rm")) {
        rm(a1);
    } else if (!strcasecmp(cmd, "register")) {
        const int idx = registry::slotFromLabel(a1);
        if (idx < 0 || argc < 3) { Serial.println("[launcher] register <slot> \"<name>\" \"<version>\""); return; }
        registry::setName(idx, argv[2], argc > 3 ? argv[3] : "");
        Serial.printf("[launcher] registered %s as \"%s\" %s\n", registry::slotLabel(idx), argv[2], argc > 3 ? argv[3] : "");
        ui::draw();
    } else if (!strcasecmp(cmd, "unregister")) {
        const int idx = registry::slotFromLabel(a1);
        if (idx < 0) { Serial.println("[launcher] unregister <slot>"); return; }
        registry::clearName(idx);
        Serial.printf("[launcher] unregistered %s\n", registry::slotLabel(idx));
        ui::draw();
    } else if (!strcasecmp(cmd, "menu")) {
        if (ui::show(a1)) { /* drawn */ }
        else if (!display::ready()) Serial.println("[launcher] display init failed");
        else Serial.println("[launcher] menu [home|settings|sd]");
    } else if (!strcasecmp(cmd, "screenshot") || !strcasecmp(cmd, "shot")) {
        if (!display::ready()) Serial.println("[screenshot] the display is not up (try `menu` first)");
        else launcher::dumpScreen(display::gfx().getBuffer(), display::W, display::H);
    } else if (!strcasecmp(cmd, "syscheck")) {
        syscheck::run(!(a1 && !strcasecmp(a1, "quick")));
        ui::draw();
    } else if (!strcasecmp(cmd, "touchflip")) {
        bool on;
        if (!onoff(a1, on)) { Serial.printf("[launcher] touchflip is %s\n", registry::touchFlip() ? "on" : "off"); return; }
        registry::setTouchFlip(on);
        hw::setTouchFlip(on);
        Serial.printf("[launcher] touchflip %s\n", on ? "on" : "off");
        ui::draw();
    } else if (!strcasecmp(cmd, "nvs")) {
        registry::dump(Serial);
    } else if (!strcasecmp(cmd, "forget")) {
        registry::forgetAll();
        Serial.println("[launcher] NVS state wiped");
        ui::draw();
    } else if (!strcasecmp(cmd, "sleep")) {
        hw::deepSleep();
    } else if (!strcasecmp(cmd, "bootloader") || !strcasecmp(cmd, "boot-mode")) {
        hw::rebootToDownloadMode();
    } else if (!strcasecmp(cmd, "reboot")) {
        Serial.println("[launcher] rebooting");
        delay(80);
        esp_restart();
    } else if (!strcasecmp(cmd, "ver")) {
        Serial.printf("[launcher] %s v%s built %s %s\n", LAUNCHER_NAME, LAUNCHER_VERSION, __DATE__, __TIME__);
    } else {
        Serial.printf("[launcher] unknown command \"%s\" (try help)\n", cmd);
    }
}

void poll() {
    while (Serial.available()) {
        const int c = Serial.read();
        if (c < 0) break;
        g_saw = true;
        if (c == '\n' || c == '\r') {
            if (g_len) {
                g_line[g_len] = 0;
                g_len = 0;
                handle(g_line);
            }
        } else if (g_len < sizeof g_line - 1) {
            g_line[g_len++] = (char)c;
        }
    }
}

}  // namespace console
