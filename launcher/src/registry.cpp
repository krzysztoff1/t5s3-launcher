#include "registry.h"

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <string.h>
#include <stdio.h>

// Must match launcher_api.h.
static const char* kNs = "launcher";

namespace registry {

static bool open(Preferences& p, bool readOnly) {
    return p.begin(kNs, readOnly);
}

const esp_partition_t* otaPartition(int index) {
    if (index < 0 || index > 15) return nullptr;
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + index), nullptr);
}

const char* slotLabel(int index) {
    static char buf[8];
    if (index < 0) return "factory";
    snprintf(buf, sizeof buf, "ota_%d", index);
    return buf;
}

int slotFromLabel(const char* label) {
    if (!label) return -2;
    if (!strcasecmp(label, "factory")) return -1;
    if (!strncasecmp(label, "ota_", 4)) label += 4;
    if (*label < '0' || *label > '9') return -2;
    return atoi(label);
}

int scan(Slot* out, int max) {
    int n = 0;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const int last = lastSlot();
    Preferences p;
    const bool haveNvs = open(p, true);

    auto add = [&](const esp_partition_t* part, int index) {
        if (!part || n >= max) return;
        Slot& s = out[n++];
        s = Slot();
        s.part = part;
        s.index = index;
        s.running = (running && running->address == part->address);
        s.last = (index >= 0 && index == last);
        esp_app_desc_t desc;
        s.valid = (esp_ota_get_partition_description(part, &desc) == ESP_OK);
        if (index < 0) {
            strlcpy(s.name, "Launcher", sizeof s.name);
            strlcpy(s.version, desc.version, sizeof s.version);
        } else if (haveNvs) {
            char key[8];
            snprintf(key, sizeof key, "n%d", index);
            String nm = p.getString(key, "");
            snprintf(key, sizeof key, "v%d", index);
            String ver = p.getString(key, "");
            strlcpy(s.name, nm.c_str(), sizeof s.name);
            strlcpy(s.version, ver.c_str(), sizeof s.version);
        }
        if (s.index >= 0 && s.valid && s.name[0] == 0) {
            // Arduino images all report "arduino-lib-builder"; only show the
            // descriptor's name when it is something an app actually set.
            if (desc.project_name[0] && strcmp(desc.project_name, "arduino-lib-builder") != 0)
                strlcpy(s.name, desc.project_name, sizeof s.name);
            else
                strlcpy(s.name, "Unregistered app", sizeof s.name);
        }
    };

    add(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr), -1);
    for (int i = 0; i < 16; ++i) {
        const esp_partition_t* part = otaPartition(i);
        if (!part) break;
        add(part, i);
    }
    if (haveNvs) p.end();
    return n;
}

bool boot(int index, esp_reset_reason_t reasonToStore) {
    const esp_partition_t* part = otaPartition(index);
    if (!part) {
        Serial.printf("[launcher] no such slot ota_%d\n", index);
        return false;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        Serial.printf("[launcher] ota_%d holds no valid app image\n", index);
        return false;
    }
    Preferences p;
    if (open(p, false)) {
        p.putUChar("last_slot", (uint8_t)index);
        p.putUChar("app_rr", (uint8_t)reasonToStore);
        p.putUChar("armed", 1);
        p.end();
    }
    esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        Serial.printf("[launcher] esp_ota_set_boot_partition failed: %d\n", (int)err);
        return false;
    }
    Serial.printf("[launcher] booting ota_%d\n", index);
    Serial.flush();
    delay(100);
    esp_restart();
    return true;
}

int lastSlot() {
    Preferences p;
    if (!open(p, true)) return -1;
    int v = p.isKey("last_slot") ? (int)p.getUChar("last_slot", 0xFF) : -1;
    p.end();
    return (v == 0xFF) ? -1 : v;
}

void setLastSlot(int index) {
    Preferences p;
    if (!open(p, false)) return;
    if (index < 0) p.remove("last_slot"); else p.putUChar("last_slot", (uint8_t)index);
    p.end();
}

bool autostart() {
    Preferences p;
    if (!open(p, true)) return true;
    bool v = p.getUChar("autostart", 1) != 0;
    p.end();
    return v;
}

void setAutostart(bool on) {
    Preferences p;
    if (!open(p, false)) return;
    p.putUChar("autostart", on ? 1 : 0);
    p.end();
}

static bool consumeFlag(const char* key) {
    Preferences p;
    if (!open(p, false)) return false;
    bool v = p.getUChar(key, 0) != 0;
    if (v) p.remove(key);
    p.end();
    return v;
}

bool consumeShowMenu() { return consumeFlag("show_menu"); }
bool consumeArmed()    { return consumeFlag("armed"); }

uint8_t crashes() {
    Preferences p;
    if (!open(p, true)) return 0;
    uint8_t v = p.getUChar("crashes", 0);
    p.end();
    return v;
}

void setCrashes(uint8_t n) {
    Preferences p;
    if (!open(p, false)) return;
    p.putUChar("crashes", n);
    p.end();
}

bool touchFlip() {
    Preferences p;
    if (!open(p, true)) return false;
    bool v = p.getUChar("tflip", 0) != 0;
    p.end();
    return v;
}

void setTouchFlip(bool on) {
    Preferences p;
    if (!open(p, false)) return;
    p.putUChar("tflip", on ? 1 : 0);
    p.end();
}

void setName(int index, const char* name, const char* version) {
    if (index < 0) return;
    Preferences p;
    if (!open(p, false)) return;
    char key[8];
    snprintf(key, sizeof key, "n%d", index);
    p.putString(key, name ? name : "");
    snprintf(key, sizeof key, "v%d", index);
    p.putString(key, version ? version : "");
    p.end();
}

void clearName(int index) {
    if (index < 0) return;
    Preferences p;
    if (!open(p, false)) return;
    char key[8];
    snprintf(key, sizeof key, "n%d", index);
    p.remove(key);
    snprintf(key, sizeof key, "v%d", index);
    p.remove(key);
    p.end();
}

void forgetAll() {
    Preferences p;
    if (!open(p, false)) return;
    p.clear();
    p.end();
}

void dump(Print& out) {
    Preferences p;
    if (!open(p, true)) { out.println("[launcher] nvs: namespace not available"); return; }
    out.printf("[launcher] nvs: last_slot=%d autostart=%u crashes=%u armed=%u show_menu=%u tflip=%u\n",
               p.isKey("last_slot") ? p.getUChar("last_slot") : -1,
               p.getUChar("autostart", 1), p.getUChar("crashes", 0),
               p.getUChar("armed", 0), p.getUChar("show_menu", 0), p.getUChar("tflip", 0));
    for (int i = 0; i < 16; ++i) {
        char key[8];
        snprintf(key, sizeof key, "n%d", i);
        if (!p.isKey(key)) continue;
        String nm = p.getString(key, "");
        snprintf(key, sizeof key, "v%d", i);
        String ver = p.getString(key, "");
        out.printf("[launcher] nvs: ota_%d name=\"%s\" version=\"%s\"\n", i, nm.c_str(), ver.c_str());
    }
    p.end();
}

}  // namespace registry
