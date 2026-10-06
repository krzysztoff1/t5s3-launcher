#include "appstore.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>

#include "registry.h"
#include "sdcard.h"

namespace appstore {

static const char* kRoot = "/apps";
// The image header (esp_image_header_t): magic at 0, chip id at 12, and at 23
// the flag saying a SHA-256 of the image follows it.
constexpr uint8_t  IMAGE_MAGIC = 0xE9;
constexpr uint16_t CHIP_ESP32S3 = 9;
constexpr size_t   HEADER_LEN = 24;
constexpr size_t   CHUNK = 8192;
// Until something has been installed and timed: SD read + erase + write.
constexpr uint16_t DEFAULT_KBPS = 150;

const char* phaseName(Phase p) {
    switch (p) {
        case READ:     return "read";
        case WRITE:    return "write";
        case VERIFY:   return "verify";
        case REGISTER: return "register";
        case START:    return "start";
        default:       return "?";
    }
}

void formatSize(uint32_t bytes, char* out, size_t cap) {
    if (bytes >= 1024u * 1024u) snprintf(out, cap, "%.1f MB", bytes / 1048576.0f);
    else snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023) / 1024));
}

uint32_t estimateMs(uint32_t bytes) {
    const uint16_t rate = registry::installRate() ? registry::installRate() : DEFAULT_KBPS;
    return (uint32_t)((uint64_t)bytes * 1000 / 1024 / rate) + 1500;   // + verify and restart
}

bool cardReady() { return sdcard::mount(); }

// --- manifest ----------------------------------------------------------------

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static bool parseHex32(const char* s, uint8_t out[32]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; ++i) {
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return false;
        out[i] = (uint8_t)strtoul(b, nullptr, 16);
    }
    return true;
}

static void readManifest(App& a) {
    char path[64];
    snprintf(path, sizeof path, "%s/%s/app.ini", kRoot, a.id);
    File f = SD.open(path, FILE_READ);
    if (!f) return;                                  // optional: the id is the name
    char line[128];
    while (f.available()) {
        const size_t n = f.readBytesUntil('\n', line, sizeof line - 1);
        line[n] = 0;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = trim(line);
        char* val = trim(eq + 1);
        if (!strcasecmp(key, "name") && *val) {
            strlcpy(a.name, val, sizeof a.name);
        } else if (!strcasecmp(key, "version")) {
            strlcpy(a.version, val, sizeof a.version);
        } else if (!strcasecmp(key, "slot")) {
            const int s = registry::slotFromLabel(val);
            if (s >= 0 && registry::otaPartition(s)) a.slot = s;
            else snprintf(a.problem, sizeof a.problem, "app.ini: slot \"%s\" is not an OTA slot", val);
        } else if (!strcasecmp(key, "sha256")) {
            a.hasSha = parseHex32(val, a.sha);
            if (!a.hasSha) strlcpy(a.problem, "app.ini: sha256 is not 64 hex digits", sizeof a.problem);
        }
    }
    f.close();
}

// Size, header sanity and the appended digest of firmware.bin.
static void readImage(App& a) {
    char path[64];
    snprintf(path, sizeof path, "%s/%s/firmware.bin", kRoot, a.id);
    File f = SD.open(path, FILE_READ);
    if (!f) { strlcpy(a.problem, "no firmware.bin", sizeof a.problem); return; }
    a.size = f.size();
    uint8_t h[HEADER_LEN];
    if (a.size < HEADER_LEN + 32 || f.read(h, HEADER_LEN) != HEADER_LEN || h[0] != IMAGE_MAGIC) {
        strlcpy(a.problem, "firmware.bin is not an app image", sizeof a.problem);
    } else if ((h[12] | (h[13] << 8)) != CHIP_ESP32S3) {
        strlcpy(a.problem, "firmware.bin is for another chip", sizeof a.problem);
    } else if (h[23] == 1 && f.seek(a.size - 32) && f.read(a.digest, 32) == 32) {
        a.hasDigest = true;
    }
    f.close();
    const esp_partition_t* part = registry::otaPartition(a.slot);
    if (!a.problem[0] && part && a.size > part->size)
        snprintf(a.problem, sizeof a.problem, "too big for %s", registry::slotLabel(a.slot));
}

static void whereInstalled(App& a) {
    a.installedIn = -1;
    if (!a.hasDigest) return;
    uint8_t d[32];
    for (int i = 0; registry::otaPartition(i); ++i) {
        if (registry::slotDigest(i, d) && memcmp(d, a.digest, 32) == 0) { a.installedIn = i; return; }
    }
}

static bool load(const char* id, App& a) {
    a = App();
    strlcpy(a.id, id, sizeof a.id);
    strlcpy(a.name, id, sizeof a.name);
    readManifest(a);
    readImage(a);
    whereInstalled(a);
    return true;
}

// /apps opens, or the card was pulled and put back since it was mounted.
static File openRoot() {
    File root = SD.open(kRoot);
    if (!root && !SD.exists("/") && sdcard::remount()) root = SD.open(kRoot);
    return root;
}

int scan(App* out, int max) {
    if (!cardReady()) return 0;
    File root = openRoot();
    if (!root || !root.isDirectory()) return 0;
    int n = 0;
    for (File e = root.openNextFile(); e && n < max; e = root.openNextFile()) {
        const bool dir = e.isDirectory();
        char id[24];
        strlcpy(id, e.name(), sizeof id);     // Arduino 2.x: the base name
        e.close();
        if (!dir || id[0] == '.') continue;
        load(id, out[n]);
        // Insertion sort by name: a handful of entries.
        for (int i = n; i > 0 && strcasecmp(out[i - 1].name, out[i].name) > 0; --i) {
            App t = out[i]; out[i] = out[i - 1]; out[i - 1] = t;
        }
        n++;
    }
    root.close();
    return n;
}

bool find(const char* id, App& out) {
    if (!id || !*id || strchr(id, '/') || !cardReady()) return false;
    char path[64];
    snprintf(path, sizeof path, "%s/%s", kRoot, id);
    File d = SD.open(path);
    if (!d && !SD.exists("/") && sdcard::remount()) d = SD.open(path);
    const bool ok = d && d.isDirectory();
    if (d) d.close();
    return ok && load(id, out);
}

// Is an image with this digest anywhere in the store? Decides whether the app
// a slot holds now could be brought back from the card after it is replaced.
static bool onCard(const uint8_t digest[32]) {
    File root = openRoot();
    if (!root) return false;
    bool found = false;
    for (File e = root.openNextFile(); e && !found; e = root.openNextFile()) {
        if (e.isDirectory()) {
            char path[64];
            snprintf(path, sizeof path, "%s/%s/firmware.bin", kRoot, e.name());
            File f = SD.open(path, FILE_READ);
            uint8_t d[32];
            if (f && f.size() > 32 && f.seek(f.size() - 32) && f.read(d, 32) == 32)
                found = memcmp(d, digest, 32) == 0;
            if (f) f.close();
        }
        e.close();
    }
    root.close();
    return found;
}

Plan plan(const App& app, int slotOverride) {
    Plan p;
    if (slotOverride < 0 && app.installedIn >= 0) {
        p.slot = app.installedIn;
        p.already = true;
        return p;
    }
    p.slot = slotOverride >= 0 ? slotOverride : app.slot;
    if (app.installedIn >= 0 && app.installedIn == p.slot) { p.already = true; return p; }

    uint8_t d[32];
    if (registry::slotDigest(p.slot, d)) {
        registry::Slot slots[8];
        const int n = registry::scan(slots, 8);
        for (int i = 0; i < n; ++i) {
            if (slots[i].index != p.slot) continue;
            snprintf(p.replaces, sizeof p.replaces, "%s%s%s", slots[i].name,
                     slots[i].version[0] ? " " : "", slots[i].version);
        }
        p.lossy = !onCard(d);
    }
    // ota_0 is OpenTrailPaper's home; anything the card cannot restore is a
    // one-way trip. Both need the user to say so.
    p.needsConfirm = p.slot == 0 || p.lossy;
    p.estimateMs = estimateMs(app.size);
    return p;
}

static void fail(char* err, size_t len, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
    Serial.printf("[install] FAILED: %s\n", err);
}

bool install(const App& app, int slot, Progress cb, void* ctx, char* err, size_t errLen) {
    auto report = [&](Phase ph, uint32_t done, uint32_t total) { if (cb) cb(ph, done, total, ctx); };
    const esp_partition_t* part = registry::otaPartition(slot);
    if (app.problem[0]) { fail(err, errLen, "%s", app.problem); return false; }
    if (!part) { fail(err, errLen, "no slot ota_%d", slot); return false; }
    if (app.size > part->size) { fail(err, errLen, "%s is too big for that slot", app.name); return false; }

    report(READ, 0, 0);
    char path[64];
    snprintf(path, sizeof path, "%s/%s/firmware.bin", kRoot, app.id);
    File f = SD.open(path, FILE_READ);
    if (!f || f.size() != app.size) { fail(err, errLen, "cannot read %s", path); return false; }
    // Flash writes come from internal RAM; esp_flash would bounce a PSRAM buffer anyway.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); fail(err, errLen, "out of memory"); return false; }

    Serial.printf("[install] %s (%u bytes) -> %s\n", app.id, (unsigned)app.size, registry::slotLabel(slot));
    const uint32_t t0 = millis();
    esp_ota_handle_t h = 0;
    // Sequential writes: erase sector by sector as the data arrives, so the
    // progress starts moving at once instead of after a multi-second erase.
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        heap_caps_free(buf); f.close();
        fail(err, errLen, "esp_ota_begin failed (%d)", (int)e);
        return false;
    }
    // From here on the slot's old app is gone: forget its label and hash.
    registry::clearName(slot);
    registry::forgetDigests();

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    uint32_t done = 0;
    bool ok = true;
    report(WRITE, 0, app.size);
    while (done < app.size) {
        const size_t want = min((size_t)CHUNK, (size_t)(app.size - done));
        const int got = f.read(buf, want);
        if (got != (int)want) { fail(err, errLen, "SD read failed at %u bytes", (unsigned)done); ok = false; break; }
        mbedtls_sha256_update_ret(&sha, buf, got);
        e = esp_ota_write(h, buf, got);
        if (e != ESP_OK) { fail(err, errLen, "flash write failed (%d)", (int)e); ok = false; break; }
        done += got;
        report(WRITE, done, app.size);
        if ((done & 0xFFFF) == 0) vTaskDelay(1);   // let the idle task (and its watchdog) run
    }
    f.close();
    heap_caps_free(buf);
    uint8_t fileSha[32];
    mbedtls_sha256_finish_ret(&sha, fileSha);
    mbedtls_sha256_free(&sha);
    if (ok && app.hasSha && memcmp(fileSha, app.sha, 32) != 0) {
        fail(err, errLen, "firmware.bin does not match the sha256 in app.ini");
        ok = false;
    }
    if (!ok) { esp_ota_abort(h); return false; }

    report(VERIFY, 0, 0);
    e = esp_ota_end(h);   // walks the image: segments, checksum, appended SHA-256
    if (e != ESP_OK) {
        fail(err, errLen, e == ESP_ERR_OTA_VALIDATE_FAILED ? "the image failed verification" : "esp_ota_end failed");
        return false;
    }
    uint8_t d[32];
    if (app.hasDigest && (!registry::slotDigest(slot, d) || memcmp(d, app.digest, 32) != 0)) {
        fail(err, errLen, "the slot does not read back as the SD image");
        return false;
    }

    report(REGISTER, 0, 0);
    registry::setName(slot, app.name, app.version);
    const uint32_t ms = millis() - t0;
    if (ms > 500) {
        const uint32_t kbps = (uint32_t)((uint64_t)app.size * 1000 / 1024 / ms);
        registry::setInstallRate((uint16_t)constrain(kbps, 10u, 5000u));
    }
    Serial.printf("[install] done: %s in %s, %u bytes in %u ms\n", app.name, registry::slotLabel(slot),
                  (unsigned)app.size, (unsigned)ms);
    return true;
}

}  // namespace appstore
