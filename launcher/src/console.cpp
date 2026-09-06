#include "console.h"

#include <Arduino.h>
#include <string.h>

#include "hw.h"
#include "registry.h"
#include "syscheck.h"
#include "ui.h"
#include "version.h"

namespace console {

static char g_line[192];
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
    Serial.println("  register <slot> \"<name>\" \"<version>\"   label a slot for the menu");
    Serial.println("  unregister <slot>             drop a slot's label");
    Serial.println("  autostart on|off              cold boot goes straight to the last app");
    Serial.println("  menu                          show the menu (brings the display up)");
    Serial.println("  syscheck [quick]              run the hardware check (quick = no touch/button steps)");
    Serial.println("  touchflip on|off              rotate touch input 180 degrees (persisted)");
    Serial.println("  nvs                           dump the launcher's NVS state");
    Serial.println("  forget                        wipe the launcher's NVS state");
    Serial.println("  sleep                         deep sleep (BOOT wakes)");
    Serial.println("  bootloader                    reboot into USB download mode");
    Serial.println("  reboot                        restart");
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
    Serial.printf("[launcher] autostart=%s last=%d\n", registry::autostart() ? "on" : "off", registry::lastSlot());
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
    const char* cmd = argv[0];
    const char* a1 = argc > 1 ? argv[1] : nullptr;

    if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
        help();
    } else if (!strcasecmp(cmd, "list") || !strcasecmp(cmd, "ls")) {
        list();
    } else if (!strcasecmp(cmd, "boot")) {
        const int idx = registry::slotFromLabel(a1);
        if (idx < 0) { Serial.println("[launcher] boot <ota_0|ota_1>"); return; }
        if (!registry::boot(idx, esp_reset_reason()))
            Serial.println("[launcher] boot failed");
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
    } else if (!strcasecmp(cmd, "autostart")) {
        bool on;
        if (!onoff(a1, on)) { Serial.printf("[launcher] autostart is %s\n", registry::autostart() ? "on" : "off"); return; }
        registry::setAutostart(on);
        Serial.printf("[launcher] autostart %s\n", on ? "on" : "off");
        ui::draw();
    } else if (!strcasecmp(cmd, "menu")) {
        if (!ui::begin()) Serial.println("[launcher] display init failed");
        else ui::draw();
    } else if (!strcasecmp(cmd, "syscheck")) {
        syscheck::run(!(a1 && !strcasecmp(a1, "quick")));
        ui::draw();
    } else if (!strcasecmp(cmd, "touchflip")) {
        bool on;
        if (!onoff(a1, on)) { Serial.printf("[launcher] touchflip is %s\n", registry::touchFlip() ? "on" : "off"); return; }
        registry::setTouchFlip(on);
        Serial.printf("[launcher] touchflip %s (takes effect after reboot)\n", on ? "on" : "off");
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
