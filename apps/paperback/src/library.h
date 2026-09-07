#pragma once
// The shelf: books embedded in the image plus /books/*.txt (and /*.txt) on the
// SD card, with per-book reading progress kept in NVS. One book is open at a
// time; SD books are read into PSRAM, embedded ones are used straight from
// flash.
#include <stddef.h>
#include <stdint.h>

namespace library {

enum Source : uint8_t { BUILTIN = 0, SDCARD = 1 };

struct Book {
    char     title[96];
    char     author[64];
    char     blurb[160];
    char     path[96];       // SD path
    char     credit[40];     // "Project Gutenberg #11", "SD card"
    char     lang[4];
    Source   src;
    uint16_t year;
    uint32_t size;           // bytes of text
    uint32_t id;             // stable hash used for the NVS key
    const uint8_t* data;     // BUILTIN: flash pointer
    const uint8_t* cover;    // BUILTIN: embedded PNG, or nullptr
    uint32_t coverLen;
    char     coverPath[96];  // SDCARD: sibling .png/.jpg, or empty
    // Progress.
    uint32_t offset;         // byte offset of the last page read
    uint8_t  percent;        // 0..100
    bool     finished;
    uint16_t seq;            // recency: higher = opened later, 0 = never
};

constexpr int MAX_BOOKS = 64;

void scan();                     // rebuild the list, load progress
int  count();
const Book& at(int i);
bool sdMounted();
int  sdCount();
// Wi-Fi credentials found on the card (/paperback/wifi.txt), or false.
bool sdWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap);

// Text of a book. SD books are loaded into PSRAM (freed by close()).
bool open(int idx, const char*& text, uint32_t& len);
void close();

void saveProgress(int idx, uint32_t offset, uint8_t percent, bool finished);
void touch(int idx);             // mark as most recently opened
int  mostRecent();               // index with the highest seq, or -1

uint32_t fnv1a(const void* data, size_t n, uint32_t seed = 2166136261u);

}  // namespace library
