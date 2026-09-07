#include "hn.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "html.h"
#include "hwio.h"
#include "json.h"
#include "settings.h"

namespace hn {

static Story*   g_front = nullptr;
static int      g_frontCount = 0;
static uint32_t g_frontAt = 0;
static bool     g_haveFront = false;

// ---- buffers -----------------------------------------------------------------

class Sink : public Stream {
public:
    char*  buf = nullptr;
    size_t len = 0, cap = 0, max = 0;
    bool   overflow = false;
    explicit Sink(size_t maxBytes) : max(maxBytes) {}
    ~Sink() { if (buf) heap_caps_free(buf); }
    char* take() { char* b = buf; buf = nullptr; return b; }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (overflow) return n;
        if (len + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 65536;
            while (ncap < len + n + 1) ncap *= 2;
            if (ncap > max + 1) ncap = max + 1;
            if (len + n + 1 > ncap) { overflow = true; return n; }
            char* nb = (char*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) { overflow = true; return n; }
            buf = nb; cap = ncap;
        }
        memcpy(buf + len, d, n);
        len += n;
        buf[len] = 0;
        return n;
    }
};

// Growable text output for the documents we build.
struct Doc {
    char* buf = nullptr; size_t len = 0, cap = 0;
    bool put(const char* s, size_t n) {
        if (len + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 32768;
            while (ncap < len + n + 1) ncap *= 2;
            char* nb = (char*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) return false;
            buf = nb; cap = ncap;
        }
        memcpy(buf + len, s, n); len += n; buf[len] = 0;
        return true;
    }
    bool puts(const char* s) { return put(s, strlen(s)); }
    bool ensure(size_t extra) {
        if (len + extra + 1 <= cap) return true;
        size_t ncap = cap ? cap * 2 : 32768;
        while (ncap < len + extra + 1) ncap *= 2;
        char* nb = (char*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
        if (!nb) return false;
        buf = nb; cap = ncap; return true;
    }
};

static char* httpGet(const char* url, const char* hName, const char* hVal, size_t maxBytes,
                     uint32_t& len, char* err, size_t errCap) {
    len = 0;
    WiFiClientSecure client;
    client.setInsecure();     // public read-only APIs; no root store on the device
    HTTPClient http;
    http.setTimeout(30000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("Paperback/0.1 (ESP32-S3 e-paper reader)");
    if (!http.begin(client, url)) { snprintf(err, errCap, "bad URL"); return nullptr; }
    if (hName) http.addHeader(hName, hVal);
    const uint32_t t0 = millis();
    const int code = http.GET();
    if (code != 200) {
        if (code < 0) snprintf(err, errCap, "%s", HTTPClient::errorToString(code).c_str());
        else snprintf(err, errCap, "HTTP %d", code);
        http.end();
        return nullptr;
    }
    Sink sink(maxBytes);
    http.writeToStream(&sink);
    http.end();
    Serial.printf("[hn] GET %.60s -> %u bytes in %lu ms%s\n", url, (unsigned)sink.len,
                  (unsigned long)(millis() - t0), sink.overflow ? " (truncated)" : "");
    if (sink.len == 0) { snprintf(err, errCap, "empty reply"); return nullptr; }
    len = (uint32_t)sink.len;
    return sink.take();
}

// ---- Wi-Fi -------------------------------------------------------------------

bool configured() { return settings::get().wifiSsid[0] != 0; }
bool online() { return WiFi.status() == WL_CONNECTED; }

bool connect(uint32_t timeoutMs) {
    if (online()) return true;
    if (!configured()) return false;
    const Settings& s = settings::get();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(s.wifiSsid, s.wifiPass);
    const uint32_t t0 = millis();
    while (!online() && millis() - t0 < timeoutMs) delay(50);
    if (online()) Serial.printf("[hn] connected to %s, %s\n", s.wifiSsid, WiFi.localIP().toString().c_str());
    else Serial.printf("[hn] could not join %s (status %d)\n", s.wifiSsid, (int)WiFi.status());
    return online();
}

void disconnect() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

// ---- time ----------------------------------------------------------------------

bool haveTime() { return time(nullptr) > 1600000000; }

bool syncClock() {
    if (!online()) return false;
    configTzTime(settings::tzPosix(settings::get().tzIdx), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    const uint32_t t0 = millis();
    while (!haveTime() && millis() - t0 < 8000) delay(100);
    if (!haveTime()) { Serial.println("[hn] NTP: no time"); return false; }
    struct tm lt;
    const time_t now = time(nullptr);
    localtime_r(&now, &lt);
    const bool ok = hwio::setClock(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    Serial.printf("[hn] clock set to %04d-%02d-%02d %02d:%02d (%s)%s\n", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, settings::tzName(settings::get().tzIdx), ok ? "" : " - RTC write failed");
    return true;
}

void ago(uint32_t unixTime, char* out, size_t cap) {
    out[0] = 0;
    if (!haveTime() || unixTime == 0) return;
    long d = (long)(time(nullptr) - (time_t)unixTime);
    if (d < 0) d = 0;
    if (d < 3600) snprintf(out, cap, "%ld min", d / 60);
    else if (d < 86400) snprintf(out, cap, "%ld h", d / 3600);
    else snprintf(out, cap, "%ld d", d / 86400);
}

// ---- front page -------------------------------------------------------------------

static void domainOf(const char* url, char* out, size_t cap) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    if (strncmp(p, "www.", 4) == 0) p += 4;
    size_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?') n++;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    for (char* c = out; *c; ++c) *c = (char)tolower((unsigned char)*c);
}

int frontCount() { return g_haveFront ? g_frontCount : 0; }
const Story& story(int i) { return g_front[i < 0 ? 0 : (i >= g_frontCount ? g_frontCount - 1 : i)]; }
uint32_t frontAgeMs() { return g_haveFront ? millis() - g_frontAt : UINT32_MAX; }

int refreshFront(char* err, size_t errCap) {
    err[0] = 0;
    if (!g_front) g_front = (Story*)heap_caps_calloc(MAX_STORIES, sizeof(Story), MALLOC_CAP_SPIRAM);
    if (!g_front) { snprintf(err, errCap, "out of memory"); return -1; }
    if (!online()) { snprintf(err, errCap, "not connected"); return -1; }

    // 1. Ranking from the official API (an array of ids).
    uint32_t rankIds[90]; int nRank = 0;
    {
        uint32_t len = 0;
        char* js = httpGet("https://hacker-news.firebaseio.com/v0/topstories.json", nullptr, nullptr, 16384, len, err, errCap);
        if (js) {
            JsonReader r(js, len);
            if (r.accept('[')) {
                while (nRank < 90) {
                    long long v;
                    if (!r.integer(v)) break;
                    rankIds[nRank++] = (uint32_t)v;
                    if (!r.accept(',')) break;
                }
            }
            heap_caps_free(js);
        }
        err[0] = 0;   // ranking is optional
    }

    // 2. Details from Algolia in one request.
    uint32_t len = 0;
    char* js = httpGet("https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=60", nullptr, nullptr, 512 * 1024, len, err, errCap);
    if (!js) return -1;

    Story* tmp = (Story*)heap_caps_calloc(60, sizeof(Story), MALLOC_CAP_SPIRAM);
    if (!tmp) { heap_caps_free(js); snprintf(err, errCap, "out of memory"); return -1; }
    int n = 0;
    JsonReader r(js, len);
    bool ok = r.accept('{');
    char key[32];
    while (ok && !r.peek('}')) {
        if (!r.string(key, sizeof key) || !r.accept(':')) { ok = false; break; }
        if (strcmp(key, "hits") == 0 && r.accept('[')) {
            while (!r.peek(']') && n < 60) {
                if (!r.accept('{')) { ok = false; break; }
                Story& s = tmp[n];
                memset(&s, 0, sizeof s);
                s.points = -1; s.comments = -1;
                bool good = true;
                while (!r.peek('}')) {
                    if (!r.string(key, sizeof key) || !r.accept(':')) { good = false; break; }
                    if (strcmp(key, "title") == 0) { if (r.peek('"')) r.string(s.title, sizeof s.title); else r.skip(); }
                    else if (strcmp(key, "url") == 0) { if (r.peek('"')) r.string(s.url, sizeof s.url); else r.skip(); }
                    else if (strcmp(key, "author") == 0) { if (r.peek('"')) r.string(s.author, sizeof s.author); else r.skip(); }
                    else if (strcmp(key, "points") == 0) { long long v; if (r.integer(v)) s.points = (int)v; else r.skip(); }
                    else if (strcmp(key, "num_comments") == 0) { long long v; if (r.integer(v)) s.comments = (int)v; else r.skip(); }
                    else if (strcmp(key, "created_at_i") == 0) { long long v; if (r.integer(v)) s.createdAt = (uint32_t)v; else r.skip(); }
                    else if (strcmp(key, "objectID") == 0) { char id[16] = {0}; if (r.peek('"')) { r.string(id, sizeof id); s.id = (uint32_t)strtoul(id, nullptr, 10); } else r.skip(); }
                    else if (strcmp(key, "story_text") == 0) { if (r.peek('"')) { r.string(nullptr, 0); s.hasText = true; } else r.skip(); }
                    else r.skip();
                    if (!r.accept(',')) break;
                }
                if (!good || !r.accept('}')) { ok = false; break; }
                if (s.title[0] && s.id) { domainOf(s.url, s.domain, sizeof s.domain); n++; }
                if (!r.accept(',')) break;
            }
            if (!r.accept(']')) ok = false;
        } else {
            r.skip();
        }
        if (!r.accept(',')) break;
    }
    heap_caps_free(js);
    if (n == 0) { heap_caps_free(tmp); if (!err[0]) snprintf(err, errCap, "no stories in reply"); return -1; }

    // 3. Order by the official ranking, unranked ones after.
    int rankOf[60];
    for (int i = 0; i < n; ++i) {
        rankOf[i] = 1000 + i;
        for (int k = 0; k < nRank; ++k) if (rankIds[k] == tmp[i].id) { rankOf[i] = k; break; }
    }
    g_frontCount = 0;
    while (g_frontCount < MAX_STORIES && g_frontCount < n) {
        int best = -1;
        for (int i = 0; i < n; ++i) if (rankOf[i] >= 0 && (best < 0 || rankOf[i] < rankOf[best])) best = i;
        if (best < 0) break;
        g_front[g_frontCount++] = tmp[best];
        rankOf[best] = -1;
    }
    heap_caps_free(tmp);
    g_frontAt = millis();
    g_haveFront = true;
    Serial.printf("[hn] front page: %d stories\n", g_frontCount);
    return g_frontCount;
}

// ---- documents -----------------------------------------------------------------------

static void metaLine(const Story& s, char* out, size_t cap) {
    char when[16]; ago(s.createdAt, when, sizeof when);
    size_t n = 0;
    n += snprintf(out + n, cap - n, "%d points", s.points < 0 ? 0 : s.points);
    if (s.comments >= 0 && n < cap) n += snprintf(out + n, cap - n, " \xC2\xB7 %d comments", s.comments);
    if (s.author[0] && n < cap) n += snprintf(out + n, cap - n, " \xC2\xB7 by %s", s.author);
    if (s.domain[0] && n < cap) n += snprintf(out + n, cap - n, " \xC2\xB7 %s", s.domain);
    if (when[0] && n < cap) snprintf(out + n, cap - n, " \xC2\xB7 %s ago", when);
}

static void docHeader(Doc& d, const Story& s) {
    d.puts(s.title); d.puts("\n\n");
    char meta[200]; metaLine(s, meta, sizeof meta);
    d.puts(meta); d.puts("\n\n* * *\n\n");
}

char* fetchArticle(const Story& s, uint32_t& len, char* err, size_t errCap) {
    len = 0; err[0] = 0;
    if (!s.url[0]) { snprintf(err, errCap, "this post has no link"); return nullptr; }
    char url[300];
    snprintf(url, sizeof url, "https://r.jina.ai/%s", s.url);
    uint32_t n = 0;
    char* body = httpGet(url, "X-Return-Format", "text", 900 * 1024, n, err, errCap);
    if (!body) return nullptr;
    Doc d;
    docHeader(d, s);
    // The proxy returns unwrapped text, one block per line: make every non-empty
    // line its own paragraph so the reflow does not glue navigation items together.
    const char* p = body; const char* e = body + n;
    while (p < e) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(e - p));
        const char* le = nl ? nl : e;
        const char* ls = p;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ++ls;
        const char* te = le;
        while (te > ls && (te[-1] == ' ' || te[-1] == '\r' || te[-1] == '\t')) --te;
        if (te > ls) { d.put(ls, (size_t)(te - ls)); d.puts("\n\n"); }
        p = nl ? nl + 1 : e;
    }
    heap_caps_free(body);
    if (d.len < 40) { if (d.buf) heap_caps_free(d.buf); snprintf(err, errCap, "the page had no readable text"); return nullptr; }
    len = (uint32_t)d.len;
    return d.buf;
}

struct CommentCtx { Doc* d; int count; int maxCount; char* tmp; size_t tmpCap; };

static void emitComment(CommentCtx& c, int depth, const char* author, uint32_t created, const char* html, size_t hlen) {
    Doc& d = *c.d;
    const int indent = depth > 6 ? 12 : depth * 2;
    char when[16]; ago(created, when, sizeof when);
    char line[80];
    snprintf(line, sizeof line, "%*s_%s%s%s_\n\n", indent, "", author[0] ? author : "[deleted]", when[0] ? " \xC2\xB7 " : "", when);
    d.puts(line);
    if (html && hlen) {
        c.tmp[0] = 0;
        htmlToText(html, hlen, c.tmp, c.tmpCap, 0, indent);
        d.puts(c.tmp);
        d.puts("\n\n");
    }
}

static bool parseCommentArray(JsonReader& r, CommentCtx& c, int depth);

static bool parseComment(JsonReader& r, CommentCtx& c, int depth) {
    if (!r.accept('{')) return false;
    char key[32];
    char author[24] = {0};
    uint32_t created = 0;
    const char* childrenAt = nullptr;
    char* text = nullptr; size_t textLen = 0;
    while (!r.peek('}')) {
        if (!r.string(key, sizeof key) || !r.accept(':')) return false;
        if (strcmp(key, "author") == 0) { if (r.peek('"')) r.string(author, sizeof author); else r.skip(); }
        else if (strcmp(key, "created_at_i") == 0) { long long v; if (r.integer(v)) created = (uint32_t)v; else r.skip(); }
        else if (strcmp(key, "children") == 0) { r.ws(); childrenAt = r.p; if (!r.skip()) return false; }
        else if (strcmp(key, "text") == 0) {
            if (r.peek('"')) {
                // Copy the raw (escaped) HTML for this comment; decode into a bounded buffer.
                text = (char*)heap_caps_malloc(c.tmpCap, MALLOC_CAP_SPIRAM);
                if (text) { r.string(text, c.tmpCap); textLen = strlen(text); } else r.string(nullptr, 0);
            } else r.skip();
        }
        else r.skip();
        if (!r.accept(',')) break;
    }
    if (!r.accept('}')) { if (text) heap_caps_free(text); return false; }
    if (c.count < c.maxCount && (textLen > 0 || author[0])) {
        if (textLen > 0) { emitComment(c, depth, author, created, text, textLen); c.count++; }
    }
    if (text) heap_caps_free(text);
    if (childrenAt && c.count < c.maxCount && depth < 8) {
        JsonReader sub(childrenAt, (size_t)(r.end - childrenAt));
        parseCommentArray(sub, c, depth + 1);
    }
    return true;
}

static bool parseCommentArray(JsonReader& r, CommentCtx& c, int depth) {
    if (!r.accept('[')) return false;
    if (r.accept(']')) return true;
    do {
        if (c.count >= c.maxCount) return true;
        if (!parseComment(r, c, depth)) return false;
    } while (r.accept(','));
    return r.accept(']');
}

char* fetchComments(const Story& s, uint32_t& len, char* err, size_t errCap) {
    len = 0; err[0] = 0;
    char url[96];
    snprintf(url, sizeof url, "https://hn.algolia.com/api/v1/items/%u", (unsigned)s.id);
    uint32_t n = 0;
    char* js = httpGet(url, nullptr, nullptr, 2 * 1024 * 1024, n, err, errCap);
    if (!js) return nullptr;

    Doc d;
    docHeader(d, s);
    CommentCtx c{&d, 0, 160, nullptr, 16384};
    c.tmp = (char*)heap_caps_malloc(c.tmpCap, MALLOC_CAP_SPIRAM);
    if (!c.tmp) { heap_caps_free(js); snprintf(err, errCap, "out of memory"); return nullptr; }

    JsonReader r(js, n);
    bool ok = r.accept('{');
    char key[32];
    const char* childrenAt = nullptr;
    char* storyText = nullptr;
    while (ok && !r.peek('}')) {
        if (!r.string(key, sizeof key) || !r.accept(':')) { ok = false; break; }
        if (strcmp(key, "children") == 0) { r.ws(); childrenAt = r.p; if (!r.skip()) ok = false; }
        else if (strcmp(key, "text") == 0 && r.peek('"')) {
            storyText = (char*)heap_caps_malloc(c.tmpCap, MALLOC_CAP_SPIRAM);
            if (storyText) r.string(storyText, c.tmpCap); else r.string(nullptr, 0);
        }
        else r.skip();
        if (!r.accept(',')) break;
    }
    if (storyText && storyText[0]) {
        c.tmp[0] = 0;
        htmlToText(storyText, strlen(storyText), c.tmp, c.tmpCap, 0, 0);
        d.puts(c.tmp); d.puts("\n\n* * *\n\n");
    }
    if (storyText) heap_caps_free(storyText);
    if (ok && childrenAt) {
        JsonReader sub(childrenAt, (size_t)(js + n - childrenAt));
        parseCommentArray(sub, c, 0);
    }
    heap_caps_free(js);
    heap_caps_free(c.tmp);
    if (c.count == 0) d.puts("No comments yet.\n");
    Serial.printf("[hn] comments: %d shown, %u bytes of text\n", c.count, (unsigned)d.len);
    len = (uint32_t)d.len;
    return d.buf;
}

}  // namespace hn
