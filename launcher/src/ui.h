#pragma once
// The launcher's screens. HOME lists the app slots and a SETTINGS row; SETTINGS
// holds the touch-flip switch, the front-light stepper and the
// actions (system check, sleep, flash mode, the last one behind a confirmation
// sheet). Touch first: every target is at least 88 px tall. The buttons still
// work: BOOT short = next, BOOT held = select; side short = next, side held =
// back; the GT911's capacitive key below the glass is back as well.
#include <Arduino.h>

namespace ui {

bool begin();                       // brings the display up and draws HOME
void draw();                        // full redraw of the current screen
// Switch to a screen by name ("home" | "settings"), the way a tap would.
// Brings the display up if it is not up yet. False if the name is unknown.
bool show(const char* name);
void poll();                        // input, idle timeout
void setNotice(const char* msg);    // banner under the status bar on HOME (crash notes etc.)
// Full-screen message on a black band, e.g. while handing over to an app.
void banner(const char* title, const char* line);
// The status band every screen wears (the system check draws it too).
void statusBar(const char* title);

}  // namespace ui
