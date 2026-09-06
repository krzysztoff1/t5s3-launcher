#pragma once
// The menu. Touch a row to act on it; or BOOT short-press moves the highlight
// and a long press selects (the side button also moves it).
#include <Arduino.h>

namespace ui {

bool begin();                       // brings the display up and draws the menu
void draw();                        // full redraw of the menu
void poll();                        // input, idle timeout
void setNotice(const char* msg);    // one line shown under the title (crash notes etc.)
// Full-screen message, e.g. while handing over to an app.
void banner(const char* title, const char* line);

}  // namespace ui
