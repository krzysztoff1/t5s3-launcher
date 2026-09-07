#include "html.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "utf8.h"

namespace {

struct Out {
    char* buf; size_t cap; size_t len;
    void put(char c) { if (len + 1 < cap) buf[len++] = c; buf[len] = 0; }
    void puts(const char* s) { while (*s) put(*s++); }
    void cp(uint32_t c) { char t[4]; const int k = utf8Put(t, c); for (int i = 0; i < k; ++i) put(t[i]); }
    bool endsWith(const char* s) const { const size_t l = strlen(s); return len >= l && memcmp(buf + len - l, s, l) == 0; }
};

struct Entity { const char* name; uint32_t cp; };
const Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"deg", 0xB0}, {"times", 0xD7}, {"middot", 0xB7}, {"bull", 0x2022},
};

uint32_t entity(const char*& p, const char* end) {
    // p points after '&'
    const char* semi = p;
    while (semi < end && semi - p < 10 && *semi != ';') ++semi;
    if (semi >= end || *semi != ';') return '&';
    uint32_t cp = 0;
    if (*p == '#') {
        cp = (p[1] == 'x' || p[1] == 'X') ? (uint32_t)strtoul(p + 2, nullptr, 16) : (uint32_t)strtoul(p + 1, nullptr, 10);
    } else {
        for (const Entity& e : kEntities) {
            const size_t l = strlen(e.name);
            if ((size_t)(semi - p) == l && strncmp(p, e.name, l) == 0) { cp = e.cp; break; }
        }
        if (!cp) return '&';
    }
    p = semi + 1;
    return cp ? cp : ' ';
}

}  // namespace

size_t htmlToText(const char* in, size_t n, char* out, size_t cap, size_t len, int indent) {
    Out o{out, cap, len};
    if (indent > 30) indent = 30;
    const char* p = in;
    const char* end = in + n;
    bool pre = false;
    bool italic = false;
    bool atLineStart = true;

    auto newPara = [&]() {
        if (o.len == 0) return;
        while (o.len > 0 && (o.buf[o.len - 1] == ' ' )) { o.len--; o.buf[o.len] = 0; }
        if (!o.endsWith("\n\n")) { if (!o.endsWith("\n")) o.put('\n'); o.put('\n'); }
        atLineStart = true;
    };
    auto emit = [&](uint32_t cp) {
        if (atLineStart) {
            for (int i = 0; i < indent; ++i) o.put(' ');
            if (pre) o.puts("    ");
            atLineStart = false;
        }
        o.cp(cp);
    };

    while (p < end) {
        if (*p == '<') {
            const char* q = p + 1;
            const bool closing = (q < end && *q == '/');
            if (closing) ++q;
            const char* nameStart = q;
            while (q < end && (isalnum((unsigned char)*q))) ++q;
            const size_t nl = (size_t)(q - nameStart);
            while (q < end && *q != '>') ++q;
            if (q >= end) break;
            p = q + 1;
            auto is = [&](const char* t) { return nl == strlen(t) && strncasecmp(nameStart, t, nl) == 0; };
            if (is("p") || is("div") || is("blockquote")) { newPara(); }
            else if (is("br")) { o.put('\n'); atLineStart = true; }
            else if (is("i") || is("em")) {
                if (!closing && !italic) { emit('_'); italic = true; }
                else if (closing && italic) { o.put('_'); italic = false; }
            }
            else if (is("pre")) { newPara(); pre = !closing; if (closing) { newPara(); } }
            else if (is("code")) { /* inline code: leave as text */ }
            continue;
        }
        uint32_t cp;
        if (*p == '&') { ++p; cp = entity(p, end); }
        else cp = utf8Next(p, end);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (pre) { o.put('\n'); atLineStart = true; }
            else if (!atLineStart) emit(' ');
            continue;
        }
        if (cp == ' ' && atLineStart && !pre) continue;
        emit(cp);
    }
    if (italic) o.put('_');
    return o.len;
}
