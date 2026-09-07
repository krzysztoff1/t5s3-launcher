#pragma once
// Owner of the e-paper: one EPD_Painter (T5 S3 preset, portrait) driving a
// 540x960 8-bit framebuffer in PSRAM. Pixel values are EPD_Painter LEVELS, not
// luminance: 0 is paper white, 3 is ink black. Drawing primitives write the
// buffer directly; paint() pushes it to the glass.
#include <stdint.h>

namespace display {

constexpr int W = 540;
constexpr int H = 960;
constexpr uint8_t WHITE = 0;
constexpr uint8_t LIGHT = 1;
constexpr uint8_t DARK  = 2;
constexpr uint8_t BLACK = 3;

// scrub: drive the panel to white first (cold start; the launcher's banner is
// on the glass). A deep-sleep wake passes false: the glass still shows our own
// last page, so a plain paint() restores the driver's idea of it.
bool begin(bool scrub);
bool ready();
uint8_t* fb();

void fill(uint8_t c);
void fillRect(int x, int y, int w, int h, uint8_t c);
void rect(int x, int y, int w, int h, uint8_t c, int thick = 1);
void hline(int x, int y, int w, uint8_t c);
void vline(int x, int y, int h, uint8_t c);
void fillRoundRect(int x, int y, int w, int h, int r, uint8_t c);
void roundRect(int x, int y, int w, int h, int r, uint8_t c, int thick = 1);
void fillCircle(int cx, int cy, int r, uint8_t c);
void circle(int cx, int cy, int r, uint8_t c);
// Textures for generated covers: 0 dots, 1 diagonal hatch, 2 horizontal rules, 3 checker.
void pattern(int x, int y, int w, int h, int kind, uint8_t c);
// Blend a rectangle towards grey: every WHITE pixel becomes `c` (for overlays).
void tint(int x, int y, int w, int h, uint8_t c);
// Scale an 8-bit grey image (0 black .. 255 white) into (x, y, w, h), dithered
// to the panel's four levels by EPD_Painter.
void drawGray8(int x, int y, int w, int h, const uint8_t* src, int srcW, int srcH);
inline void pixel(int x, int y, uint8_t c);

// FAST trades deeper blacks for a quicker page turn (7 passes instead of 13).
void setFast(bool fast);
void paint();                                   // delta update, blocks until the drive finishes
void clearRegion(int x, int y, int w, int h, bool hard);   // drive a region to white (ghost removal)
void hardClear(int passes = 1);                 // full-panel scrub; also whitens the framebuffer
void scrub();                                   // full-panel scrub that leaves the framebuffer alone
void end();                                     // release the panel before deep sleep

}  // namespace display

inline void display::pixel(int x, int y, uint8_t c) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    fb()[y * W + x] = c;
}
