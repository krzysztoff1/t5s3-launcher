#pragma once
// Shared widgets: header bar, buttons, progress bars, generated covers, toasts,
// and the tap-target list every screen fills while it draws.
#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "library.h"

namespace ui {

constexpr int MARGIN = 24;
constexpr int CONTENT_W = 540 - 2 * MARGIN;   // 492
constexpr int GUTTER = 12;                    // between buttons in a row
constexpr int HEADER_BASE = 52;     // baseline of the header title
constexpr int HEADER_H = 84;        // header band incl. its rule; content starts at >= 96
constexpr int HEADER_HIT_H = 96;    // the back target reaches this far down
// Smallest touch target, either way: a thumb on a 4.7" panel (the same floor
// OpenTrailPaper uses). Hit rects may be bigger than what is drawn, never smaller.
constexpr int TOUCH_MIN = 88;
// Bottom action bar: one row of TOUCH_MIN-tall buttons, 16 px off the bottom
// edge, on the 24 px margins. Screens that use it end their content above it.
constexpr int BAR_H = TOUCH_MIN;
constexpr int BAR_Y = 960 - 16 - BAR_H;   // 856..944

// Tap targets.
void hitsClear();
void hit(int x, int y, int w, int h, int id, int arg = 0);
int  hitTest(int x, int y, int* arg);   // -1 when nothing

// Chrome.
void header(const char* title, const char* backLabel = nullptr, int backId = -1);
void status(int rx, int baseline);      // clock + battery, right-aligned
void batteryIcon(int x, int y, int pct, bool usb);
void footer(const char* text);          // small centred hint at the bottom
void sectionLabel(int x, int baseline, const char* text);   // small caps label

// Controls. Every one of these registers a hit rect at least TOUCH_MIN tall.
void button(int x, int y, int w, int h, const char* label, bool primary, int id, int arg = 0, bool enabled = true);
void chevron(int cx, int cy, int size, bool right, uint8_t ink, int thick = 2);
// Outlined square with a chevron; w/h should be TOUCH_MIN.
void chevronButton(int x, int y, int w, int h, bool right, int id, int arg, bool enabled = true);
// Outlined button with an icon above a small label (bottom-bar style).
enum Icon : uint8_t { ICON_HN = 0, ICON_GEAR, ICON_REFRESH, ICON_MOON };
void iconButton(int x, int y, int w, int h, Icon icon, const char* label, int id, int arg = 0, bool enabled = true);
// Segmented control: `count` equal cells, the active one filled. Each cell
// registers `id` with its index as arg.
void tabs(int x, int y, int w, int h, const char* const* labels, int count, int active, int id);
void progressBar(int x, int y, int w, int h, float frac, const uint16_t* ticksPermille = nullptr, int nTicks = 0);
void checkBadge(int cx, int cy, int r);
void dots(int cx, int y, int count, int active);   // page indicator

// Content.
void cover(int x, int y, int w, int h, const library::Book& b, bool selected);
void hnLogo(int x, int y, int size);
void iconMoon(int cx, int cy, int r, uint8_t ink);
void iconSun(int cx, int cy, int r, uint8_t ink);
void iconGear(int cx, int cy, int r, uint8_t ink);
void iconList(int cx, int cy, int r, uint8_t ink);
void iconRefresh(int cx, int cy, int r, uint8_t ink);

// Draws a centred card over the current frame and paints it. Used before slow work.
void toast(const char* line1, const char* line2 = nullptr);

void formatDuration(char* out, size_t cap, float seconds);   // "2 h 10 min", "25 min"
int  wrapLines(const char* s, font::Face f, float px, int w, int maxLines);   // lines needed (capped)

}  // namespace ui
