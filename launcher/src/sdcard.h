#pragma once
// The SD card, shared by the system check, the console's file commands and the
// SD app store. The card sits on the SPI bus it shares with the LoRa radio, so
// the radio's chip select is parked high before anything talks to it.
#include <Arduino.h>

namespace sdcard {

void spiBegin();          // the shared SPI bus, both chip selects parked; idempotent
// Mount the card if it is not mounted. A failed attempt is remembered for a
// few seconds so a screen redraw without a card does not stall on it; `force`
// tries anyway (the system check).
bool mount(bool force = false);
bool mounted();
void unmount();
// The driver caches the card type, so a pulled card still reads "mounted";
// callers whose open() fails unexpectedly remount through this.
bool remount();
// mkdir every directory on the way to `path` (SD.mkdir makes one level).
void mkdirsFor(const char* path);

}  // namespace sdcard
