#pragma once
// Everything on the board that is not the e-paper: I2C bus, XL9555 expander
// (side button), BQ25896 charger (USB present?), BQ27220 gauge (battery %),
// GT911 touch, PCF8563 clock, PT4103 front light, deep sleep.
#include <Arduino.h>

namespace hwio {

void begin();                    // Wire + expander + charger + gauge; all non-fatal

bool sideButtonPressed();        // expander pin 10, LOW = pressed (double-read)
void rearmSideButton();          // EPD_Painter's power control turns the pin into an output
bool bootButtonPressed();        // GPIO0, LOW = pressed

bool usbPowered();               // charger VBUS present (false without a charger)
int  batteryPercent();           // -1 if unknown

bool touchBegin();
bool touchOk();
// One touch point in canvas coordinates (540x960 portrait), honouring the
// launcher's persisted 180-degree flip. False when nothing is pressed.
bool touchRead(int& x, int& y);
// The capacitive Home key below the glass is a GT911 "key", reported while
// touchRead() polls the controller. True once per press (400 ms debounce).
// Unverified on hardware: nothing may depend on it, it only shortcuts "back".
bool homeKeyPressed();

// PCF8563 wall clock; false when absent or never set.
bool clock(int& hour, int& minute);
bool setClock(int year, int month, int day, int hour, int minute, int second);

void frontLight(uint8_t duty);   // PT4103 PWM duty 0..255

[[noreturn]] void deepSleep();   // wake on BOOT button; caller has already drawn the sleep screen
[[noreturn]] void rebootToDownloadMode();

}  // namespace hwio
