#include "library.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "board.h"
#include "builtin_books.h"

namespace library {

static Book g_books[MAX_BOOKS];
static int  g_count = 0;
static bool g_sd = false;
static int  g_sdCount = 0;
static char* g_loaded = nullptr;   // PSRAM copy of the open SD book
static uint16_t g_seq = 0;

struct __attribute__((packed)) Record {
    uint32_t offset;
    uint8_t  percent;
    uint8_t  finished;
    uint16_t seq;
};

uint32_t fnv1a(const void* data, size_t n, uint32_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = seed;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void key(char* out, uint32_t id) { snprintf(out, 12, "b%08x", (unsigned)id); }

static void loadProgress(Book& b) {
    Preferences p;
    b.offset = 0; b.percent = 0; b.finished = false; b.seq = 0;
    if (!p.begin("paperback", true)) return;
    char k[12]; key(k, b.id);
    Record r{};
    if (p.getBytes(k, &r, sizeof r) == sizeof r) {
        b.offset = r.offset; b.percent = r.percent > 100 ? 100 : r.percent;
        b.finished = r.finished != 0; b.seq = r.seq;
        if (r.seq > g_seq) g_seq = r.seq;
    }
    p.end();
}

static void storeProgress(const Book& b) {
    Preferences p;
    if (!p.begin("paperback", false)) return;
    char k[12]; key(k, b.id);
    Record r{b.offset, b.percent, (uint8_t)(b.finished ? 1 : 0), b.seq};
    p.putBytes(k, &r, sizeof r);
    p.end();
}

int count() { return g_count; }
const Book& at(int i) { return g_books[i < 0 ? 0 : (i >= g_count ? g_count - 1 : i)]; }
bool sdMounted() { return g_sd; }
int sdCount() { return g_sdCount; }

static bool g_spi = false;
static bool mountSd() {
    if (g_sd) return true;
    if (!g_spi) {
        pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);   // keep the radio off the bus
        pinMode(BOARD_SD_CS, OUTPUT);   digitalWrite(BOARD_SD_CS, HIGH);
        SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI, BOARD_SD_CS);
        g_spi = true;
    }
    g_sd = SD.begin(BOARD_SD_CS, SPI, 20000000);
    if (!g_sd) g_sd = SD.begin(BOARD_SD_CS, SPI, 4000000);
    if (g_sd) Serial.printf("[library] SD card: %llu MB\n", SD.cardSize() / (1024 * 1024));
    else Serial.println("[library] no SD card");
    return g_sd;
}

static bool endsWithTxt(const char* n) {
    const size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".txt") == 0;
}

// "Author - Title.txt" -> author/title; otherwise the file name is the title.
static void namesFromFile(const char* fname, Book& b) {
    char base[96];
    strlcpy(base, fname, sizeof base);
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
    for (char* c = base; *c; ++c) if (*c == '_') *c = ' ';
    const char* sep = strstr(base, " - ");
    if (sep) {
        size_t al = (size_t)(sep - base);
        if (al >= sizeof b.author) al = sizeof b.author - 1;
        memcpy(b.author, base, al); b.author[al] = 0;
        strlcpy(b.title, sep + 3, sizeof b.title);
    } else {
        strlcpy(b.title, base, sizeof b.title);
        b.author[0] = 0;
    }
}

// Read "Title:" / "Author:" from a Project Gutenberg header when present.
static void sniffGutenberg(File& f, Book& b) {
    char head[3072];
    const size_t n = f.read((uint8_t*)head, sizeof head - 1);
    head[n] = 0;
    if (!strstr(head, "Project Gutenberg")) return;
    auto grab = [&](const char* tag, char* out, size_t cap) {
        const char* p = strstr(head, tag);
        if (!p) return;
        p += strlen(tag);
        while (*p == ' ') ++p;
        const char* e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        while (e > p && (e[-1] == '\r' || e[-1] == ' ')) --e;
        size_t l = (size_t)(e - p);
        if (l >= cap) l = cap - 1;
        if (l) { memcpy(out, p, l); out[l] = 0; }
    };
    grab("\nTitle:", b.title, sizeof b.title);
    grab("\nAuthor:", b.author, sizeof b.author);
}

static void scanDir(const char* dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f && g_count < MAX_BOOKS; f = d.openNextFile()) {
        if (f.isDirectory()) { f.close(); continue; }
        const char* name = f.name();
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (base[0] == '.' || base[0] == '_' || !endsWithTxt(base) || f.size() < 512) { f.close(); continue; }
        Book& b = g_books[g_count];
        memset(&b, 0, sizeof b);
        b.src = SDCARD;
        snprintf(b.path, sizeof b.path, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, base);
        b.size = (uint32_t)f.size();
        namesFromFile(base, b);
        sniffGutenberg(f, b);
        // Cover art: the same file name with an image extension.
        {
            char probe[112];
            const char* exts[] = {".png", ".jpg", ".jpeg"};
            for (const char* ext : exts) {
                snprintf(probe, sizeof probe, "%s", b.path);
                char* dot = strrchr(probe, '.');
                if (!dot) break;
                strlcpy(dot, ext, sizeof probe - (size_t)(dot - probe));
                if (SD.exists(probe)) { strlcpy(b.coverPath, probe, sizeof b.coverPath); break; }
            }
        }
        strlcpy(b.credit, "SD card", sizeof b.credit);
        strlcpy(b.lang, "", sizeof b.lang);
        b.id = fnv1a(b.path, strlen(b.path), fnv1a(&b.size, sizeof b.size));
        loadProgress(b);
        g_count++;
        g_sdCount++;
        f.close();
    }
    d.close();
}

void scan() {
    g_count = 0;
    g_sdCount = 0;
    g_seq = 0;
    for (size_t i = 0; i < kBuiltinBookCount && g_count < MAX_BOOKS; ++i) {
        const BuiltinBook& s = kBuiltinBooks[i];
        Book& b = g_books[g_count++];
        memset(&b, 0, sizeof b);
        strlcpy(b.title, s.title, sizeof b.title);
        strlcpy(b.author, s.author, sizeof b.author);
        strlcpy(b.blurb, s.blurb, sizeof b.blurb);
        strlcpy(b.credit, s.source, sizeof b.credit);
        strlcpy(b.lang, s.lang, sizeof b.lang);
        b.year = s.year;
        b.src = BUILTIN;
        b.data = s.start;
        b.size = (uint32_t)(s.end - s.start);
        b.cover = s.coverStart;
        b.coverLen = s.coverStart ? (uint32_t)(s.coverEnd - s.coverStart) : 0;
        b.id = fnv1a(b.title, strlen(b.title), fnv1a(b.author, strlen(b.author)));
        loadProgress(b);
    }
    if (mountSd()) {
        scanDir("/books");
        scanDir("/Books");
        scanDir("/");
    }
    Serial.printf("[library] %d books (%d on SD)\n", g_count, g_sdCount);
}

bool sdWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
    if (!g_sd) return false;
    File f = SD.open("/paperback/wifi.txt");
    if (!f) return false;
    char buf[200];
    const size_t n = f.read((uint8_t*)buf, sizeof buf - 1);
    f.close();
    buf[n] = 0;
    char* nl = strchr(buf, '\n');
    if (!nl) return false;
    *nl = 0;
    char* line1 = buf; char* line2 = nl + 1;
    char* nl2 = strchr(line2, '\n'); if (nl2) *nl2 = 0;
    auto trim = [](char* s) { size_t l = strlen(s); while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0; while (*s == ' ') ++s; return s; };
    line1 = trim(line1); line2 = trim(line2);
    if (!*line1) return false;
    strlcpy(ssid, line1, ssidCap);
    strlcpy(pass, line2, passCap);
    return true;
}

// Cut Project Gutenberg boilerplate: text between the START and END markers.
static void trimGutenberg(const char*& text, uint32_t& len) {
    static const char* startTag = "*** START OF";
    static const char* endTag = "*** END OF";
    const char* s = text;
    const char* e = text + len;
    // Only look in the first 8 KB for the start marker.
    const uint32_t look = len < 8192 ? len : 8192;
    for (const char* p = s; p + 12 < s + look; ++p) {
        if (*p == '*' && strncmp(p, startTag, 12) == 0) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(e - p));
            if (nl) { text = nl + 1; len = (uint32_t)(e - text); }
            break;
        }
    }
    s = text; e = text + len;
    // and the last 24 KB for the end marker
    const char* from = len > 24576 ? e - 24576 : s;
    for (const char* p = from; p + 10 < e; ++p) {
        if (*p == '*' && strncmp(p, endTag, 10) == 0) { len = (uint32_t)(p - s); break; }
    }
}

bool open(int idx, const char*& text, uint32_t& len) {
    close();
    if (idx < 0 || idx >= g_count) return false;
    Book& b = g_books[idx];
    if (b.src == BUILTIN) {
        text = (const char*)b.data;
        len = b.size;
        return true;
    }
    if (!g_sd) return false;
    File f = SD.open(b.path, FILE_READ);
    if (!f) { Serial.printf("[library] cannot open %s\n", b.path); return false; }
    const uint32_t size = (uint32_t)f.size();
    if (size > 6u * 1024 * 1024) { Serial.println("[library] file too large (6 MB max)"); f.close(); return false; }
    g_loaded = (char*)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!g_loaded) { f.close(); return false; }
    uint32_t got = 0;
    const uint32_t t0 = millis();
    while (got < size) {
        const int n = f.read((uint8_t*)g_loaded + got, size - got > 32768 ? 32768 : size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    f.close();
    g_loaded[got] = 0;
    Serial.printf("[library] read %u bytes from %s in %lu ms\n", (unsigned)got, b.path, (unsigned long)(millis() - t0));
    text = g_loaded;
    len = got;
    trimGutenberg(text, len);
    return got > 0;
}

void close() {
    if (g_loaded) { heap_caps_free(g_loaded); g_loaded = nullptr; }
}

void saveProgress(int idx, uint32_t offset, uint8_t percent, bool finished) {
    if (idx < 0 || idx >= g_count) return;
    Book& b = g_books[idx];
    if (b.offset == offset && b.percent == percent && b.finished == finished) return;
    b.offset = offset; b.percent = percent; b.finished = finished;
    storeProgress(b);
}

void touch(int idx) {
    if (idx < 0 || idx >= g_count) return;
    Book& b = g_books[idx];
    if (b.seq == g_seq && g_seq != 0) return;
    b.seq = ++g_seq;
    storeProgress(b);
}

int mostRecent() {
    int best = -1;
    for (int i = 0; i < g_count; ++i) {
        if (g_books[i].seq && (best < 0 || g_books[i].seq > g_books[best].seq)) best = i;
    }
    return best;
}

}  // namespace library
