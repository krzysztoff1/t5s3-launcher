#pragma once
// Thin owner of the e-paper: one EPD_Painter (T5 S3 preset, portrait) driving a
// 540x960 Adafruit GFX canvas held in PSRAM. Colours are EPD_Painter LEVELS,
// not luminance: 0 is paper white, 3 is ink black.
#include <Adafruit_GFX.h>
#include <stddef.h>

namespace display {

constexpr uint8_t WHITE = 0;
constexpr uint8_t LIGHT = 1;
constexpr uint8_t DARK  = 2;
constexpr uint8_t BLACK = 3;

constexpr int W = 540;
constexpr int H = 960;

// The type ramp. Sizes are what Adafruit GFX ships; caps are roughly 34 / 25 /
// 17 / 13 px tall, which is what the row heights in ui.cpp were sized against.
extern const GFXfont* const FONT_HERO;    // bold 24pt: banners, sheet headlines
extern const GFXfont* const FONT_TITLE;   // bold 18pt: list-row titles, button text
extern const GFXfont* const FONT_ITEM;    // bold 12pt: status bar, setting labels
extern const GFXfont* const FONT_BODY;    // regular 12pt: subtitles, body copy
extern const GFXfont* const FONT_LABEL;   // bold 9pt: tracked captions
extern const GFXfont* const FONT_SMALL;   // regular 9pt: footers, details

bool begin();                   // idempotent; needs Wire.begin() first
bool ready();
GFXcanvas8& gfx();
void paint();                   // push the canvas and wait for the drive to finish
void hardClear(int passes = 2); // scrub ghosts (slow)
void end();                     // release the panel before deep sleep

// Text helpers. y is the BASELINE for custom fonts.
void text(int x, int y, const char* s, const GFXfont* font, uint8_t color = BLACK);
void textCentered(int cx, int y, const char* s, const GFXfont* font, uint8_t color = BLACK);
void textRight(int rx, int y, const char* s, const GFXfont* font, uint8_t color = BLACK);
int  textWidth(const char* s, const GFXfont* font);
int  capHeight(const GFXfont* font);      // ink height of 'H', for vertical centring
// Truncates with an ellipsis to fit maxWidth pixels.
void textFit(int x, int y, const char* s, const GFXfont* font, int maxWidth, uint8_t color = BLACK);
// Uppercase ASCII copy (the row titles and captions are set in caps).
void upper(char* out, size_t cap, const char* in);

// Tracked-out uppercase caption: 3 px of letterspacing on top of each glyph's
// advance, the way OpenTrailPaper sets its labels. x is the left edge.
int  labelWidth(const char* s, const GFXfont* font);
void label(int x, int y, const char* s, const GFXfont* font, uint8_t color = BLACK);
void labelCentered(int cx, int y, const char* s, const GFXfont* font, uint8_t color = BLACK);

// Shapes.
void frame(int x, int y, int w, int h, int thick, uint8_t color = BLACK);   // outline
void rule(int x, int y, int w, int thick, uint8_t color = BLACK);           // horizontal bar
void tone50(int x, int y, int w, int h);          // 2 px checker: the scrim behind a sheet
void batteryIcon(int rightX, int cy, int pct, bool charging);   // 39 x 18, right-anchored

}  // namespace display
