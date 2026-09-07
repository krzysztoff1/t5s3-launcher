#pragma once
// Cover art: PNGs embedded with the sample books, or a PNG/JPEG next to a book
// on the SD card. Decoded once into an 8-bit grey cache in PSRAM and drawn
// scaled with EPD_Painter's dithering (four greys against the panel's curve).
#include <stdint.h>

namespace library { struct Book; }

namespace covers {

bool has(const library::Book& b);
// Draws the art fitted into (x, y, w, h) on a white surround with a thin frame.
// False when the book has no usable image; the caller draws its own cover.
bool draw(int x, int y, int w, int h, const library::Book& b);
void flush();

}  // namespace covers
