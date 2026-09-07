#pragma once
// Screen flow and input. One document is open at a time; the reader draws
// into the framebuffer and paints; every other screen is a full repaint too.
#include <Arduino.h>

namespace app {

void begin(bool wakeFromSleep);
void poll();

// Console entry points.
void showLibrary();
bool openBook(int idx);
bool nextPage();
bool prevPage();
bool gotoPage(int page1);          // 1-based
bool gotoPercent(int pct);
void showMenu();
void sleepNow();
void refreshScreen();
void openHackerNews();
void settingsChanged(bool relayout);
void status(Print& out);

}  // namespace app
