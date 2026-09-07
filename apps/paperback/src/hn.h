#pragma once
// Hacker News over Wi-Fi: the front page (Firebase ranking + Algolia details),
// article bodies as plain text through the r.jina.ai reader proxy, and comment
// threads from the Algolia items API converted to reader text.
#include <stddef.h>
#include <stdint.h>

namespace hn {

struct Story {
    uint32_t id;
    char     title[140];
    char     url[220];
    char     domain[48];
    char     author[24];
    int      points;
    int      comments;
    uint32_t createdAt;   // unix seconds
    bool     hasText;     // Ask/Show HN: the post itself has text
};
constexpr int MAX_STORIES = 30;

bool configured();                 // Wi-Fi credentials present
bool online();
bool connect(uint32_t timeoutMs);  // blocking; returns online()
void disconnect();                 // radio off

// Front page. Returns the story count, or -1 with err filled.
int  refreshFront(char* err, size_t errCap);
int  frontCount();
const Story& story(int i);
uint32_t frontAgeMs();             // millis since the last successful refresh (UINT32_MAX if never)

// Reader documents in PSRAM (caller frees with heap_caps_free). nullptr + err on failure.
char* fetchArticle(const Story& s, uint32_t& len, char* err, size_t errCap);
char* fetchComments(const Story& s, uint32_t& len, char* err, size_t errCap);

bool syncClock();                  // NTP -> system time -> PCF8563, in the configured zone
void ago(uint32_t unixTime, char* out, size_t cap);   // "3 h", "2 d", "" when the clock is unknown
bool haveTime();

}  // namespace hn
