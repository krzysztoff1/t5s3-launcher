// Minimal app for the LilyGO T5S3 4.7" e-paper PRO under t5s3-launcher.
//
// Shows a screen, and goes back to the launcher when BOOT is held for a second
// or the console receives `launcher`. Everything launcher-specific is the two
// launcher:: calls; the rest is an ordinary EPD_Painter sketch.
#include <Arduino.h>
#include <Wire.h>

#include "launcher_api.h"

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"
#include "EPD_Painter_Adafruit.h"
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans12pt7b.h>

static const int BOARD_SDA = 39, BOARD_SCL = 40, BOARD_BOOT_BTN = 0;

// Level indices, not luminance: 0 = paper white, 3 = ink black.
static const uint8_t WHITE = 0, BLACK = 3;

static EPD_PainterAdafruit* gfx = nullptr;

static void draw() {
    gfx->fillScreen(WHITE);
    gfx->fillRect(0, 0, 540, 96, BLACK);
    gfx->setFont(&FreeSansBold24pt7b);
    gfx->setTextColor(WHITE);
    gfx->setCursor(24, 62);
    gfx->print(APP_NAME);
    gfx->setFont(&FreeSans12pt7b);
    gfx->setTextColor(BLACK);
    gfx->setCursor(24, 160);
    gfx->print("Version " APP_VERSION);
    gfx->setCursor(24, 220);
    gfx->print("Hold BOOT for 1 s to return to the launcher.");
    gfx->paint();
}

void setup() {
    launcher::handoff(APP_NAME, APP_VERSION);   // must stay the first line

    Serial.begin(115200);
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);
    Wire.begin(BOARD_SDA, BOARD_SCL);

    EPD_Painter::Config cfg = EPD_LILYGO_T5_S3_GPS_PRESET;
    cfg.i2c.wire = &Wire;                       // share OUR bus; see launcher/src/display.cpp
    gfx = new EPD_PainterAdafruit(cfg, /*portrait=*/true);
    gfx->setAutoShutdown(false);                // or begin() may never return
    if (!gfx->begin()) {
        Serial.println("[app] display init failed");
        return;
    }
    draw();
    Serial.printf("[app] %s %s running (reset: %d)\n", APP_NAME, APP_VERSION, (int)launcher::previousResetReason());
}

void loop() {
    // Console: `launcher` hands back.
    static char line[32]; static size_t n = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[n] = 0; n = 0;
            if (!strcasecmp(line, "launcher")) launcher::returnToLauncher();
        } else if (n < sizeof line - 1) line[n++] = c;
    }
    // BOOT held for a second hands back.
    static uint32_t downAt = 0;
    if (digitalRead(BOARD_BOOT_BTN) == LOW) {
        if (!downAt) downAt = millis();
        else if (millis() - downAt > 1000) launcher::returnToLauncher();
    } else {
        downAt = 0;
    }
    delay(10);
}
