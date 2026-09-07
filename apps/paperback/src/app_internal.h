#pragma once
// Shared between app.cpp (library, reader, menus) and app_hn.cpp (Hacker News).
#include <stdint.h>

#include "layout.h"
#include "textmodel.h"

namespace app {

enum Screen : uint8_t { LIBRARY = 0, READER, MENU, SETTINGS, CHAPTERS, END, HINT, HN_LIST, HN_STORY, WIFI_INFO };

enum HitId {
    H_BACK = 1, H_HN_BANNER, H_HERO, H_BOOK, H_LIB_PREV, H_LIB_NEXT, H_SETTINGS, H_LAUNCHER,
    H_MENU_CONTINUE, H_MENU_CHAPTERS, H_MENU_SETTINGS, H_MENU_LIBRARY, H_MENU_BACKTO, H_MENU_SWITCH,
    H_MENU_REFRESH, H_MENU_SLEEP, H_LIGHT_MINUS, H_LIGHT_PLUS, H_PROGRESS,
    H_ROW_PREV, H_ROW_NEXT, H_DONE,
    H_CHAPTER, H_CH_PREV, H_CH_NEXT,
    H_END_LIBRARY, H_END_AGAIN,
    H_HINT_OK,
    H_HN_STORY, H_HN_REFRESH, H_HN_PREV, H_HN_NEXT, H_HN_ARTICLE, H_HN_COMMENTS, H_WIFI_RETRY,
    H_SET_TAB,
};

struct Doc {
    bool     open = false;
    int      book = -1;          // library index, -1 for web documents
    int      hnStory = -1;       // Hacker News story index for web documents
    bool     isComments = false;
    char     title[140] = {0};
    char     subtitle[120] = {0};
    const char* text = nullptr;
    uint32_t len = 0;
    bool     ownsText = false;
    text::Model model;
    layout::Pagination pages;
    uint32_t page = 0;
    int      jumpFrom = -1;      // page to offer "back to" after a jump
    uint16_t ticks[64];
    int      tickCount = 0;
};

extern Screen g_screen;
extern Doc    g_doc;

// app.cpp
void setScreen(Screen s);
void drawLibrary();
void drawReader(bool paintNow = true);
void enterReader();
bool openText(const char* text, uint32_t len, bool ownsText, const char* title, const char* subtitle, uint32_t startOffset);
void closeDoc();
void paintScreen();
void goToLibrary();

// app_hn.cpp
void hnEnter();
void hnDrawList();
void hnDrawStory();
void hnDrawWifiInfo();
bool hnTap(int id, int arg);
void hnButton(bool boot);
void hnSwipe(int dir);
void hnLeave();
bool hnOpen(int story, bool comments);

}  // namespace app
