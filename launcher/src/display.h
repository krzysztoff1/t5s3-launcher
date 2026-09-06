#pragma once
// Thin owner of the e-paper: one EPD_Painter (T5 S3 preset, portrait) driving a
// 540x960 Adafruit GFX canvas held in PSRAM. Colours are EPD_Painter LEVELS,
// not luminance: 0 is paper white, 3 is ink black.
#include <Adafruit_GFX.h>

namespace display {

constexpr uint8_t WHITE = 0;
constexpr uint8_t LIGHT = 1;
constexpr uint8_t DARK  = 2;
constexpr uint8_t BLACK = 3;

constexpr int W = 540;
constexpr int H = 960;

extern const GFXfont* const FONT_TITLE;   // bold 24pt
extern const GFXfont* const FONT_ITEM;    // bold 12pt
extern const GFXfont* const FONT_BODY;    // regular 12pt
extern const GFXfont* const FONT_SMALL;   // regular 9pt

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
// Truncates with an ellipsis to fit maxWidth pixels.
void textFit(int x, int y, const char* s, const GFXfont* font, int maxWidth, uint8_t color = BLACK);

}  // namespace display
