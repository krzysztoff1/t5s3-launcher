#pragma once
// A pull parser for the two JSON shapes we read from Hacker News. No DOM: the
// caller walks objects and arrays and skips what it does not want.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utf8.h"

struct JsonReader {
    const char* p;
    const char* end;

    static uint32_t hex4(const char* s) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        }
        return v;
    }

    JsonReader(const char* s, size_t n) : p(s), end(s + n) {}

    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool peek(char c) { ws(); return p < end && *p == c; }
    bool accept(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
    bool atEnd() { ws(); return p >= end; }

    // Reads a string into out (UTF-8, escapes decoded, truncated to cap-1). Passing
    // out = nullptr just skips it.
    bool string(char* out, size_t cap) {
        if (!accept('"')) return false;
        size_t n = 0;
        while (p < end && *p != '"') {
            uint32_t cp;
            if (*p == '\\') {
                ++p;
                if (p >= end) return false;
                const char e = *p++;
                switch (e) {
                    case 'n': cp = '\n'; break;
                    case 't': cp = '\t'; break;
                    case 'r': cp = '\r'; break;
                    case 'b': cp = '\b'; break;
                    case 'f': cp = '\f'; break;
                    case 'u': {
                        if (end - p < 4) return false;
                        cp = hex4(p);
                        p += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            const uint32_t lo = hex4(p + 2);
                            if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                        }
                        break;
                    }
                    default: cp = (uint8_t)e; break;
                }
            } else {
                cp = utf8Next(p, end);
            }
            if (out) {
                char tmp[4];
                const int k = utf8Put(tmp, cp);
                if (n + k < cap) { memcpy(out + n, tmp, k); n += k; }
            }
        }
        if (out && cap) out[n] = 0;
        return accept('"');
    }

    bool number(double& v) {
        ws();
        char* e = nullptr;
        v = strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }

    bool integer(long long& v) {
        double d;
        if (!number(d)) return false;
        v = (long long)d;
        return true;
    }

    bool literal(bool* isNull = nullptr) {
        ws();
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; if (isNull) *isNull = true; return true; }
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; return true; }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; return true; }
        return false;
    }

    bool skip() {
        ws();
        if (p >= end) return false;
        if (*p == '"') return string(nullptr, 0);
        if (*p == '{') {
            ++p;
            if (accept('}')) return true;
            do {
                if (!string(nullptr, 0) || !accept(':') || !skip()) return false;
            } while (accept(','));
            return accept('}');
        }
        if (*p == '[') {
            ++p;
            if (accept(']')) return true;
            do { if (!skip()) return false; } while (accept(','));
            return accept(']');
        }
        double d;
        if (number(d)) return true;
        return literal();
    }
};
