#pragma once
// Shared peripherals the launcher touches on every boot: I2C bus, the XL9555
// expander (side button, radio rail), the BQ25896 charger (USB present?), the
// BQ27220 gauge (battery %), the GT911 touch panel, buttons, backlight, sleep.
#include <Arduino.h>

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include <ExtensionIOXL9555.hpp>
#include <TouchDrvGT911.hpp>
#include <bq27220.h>

namespace hw {

void beginI2C();                 // Wire + expander + charger + gauge; all non-fatal
bool expanderOk();
bool chargerOk();
bool gaugeOk();

bool sideButtonPressed();        // expander IO12, LOW = pressed
// EPD_Painter's power control configures the expander's whole upper port as
// outputs, the side button's pin included. Call after the display is up.
void rearmSideButton();
bool bootButtonPressed();        // GPIO0, LOW = pressed
void radioPower(bool on);        // GPS + LoRa 3V3 rail
bool usbPowered();               // charger VBUS present (false if no charger)
int  batteryPercent();           // -1 if unknown
uint16_t batteryMillivolts();    // 0 if unknown

bool touchBegin();
bool touchOk();
// One touch point in canvas coordinates (540x960 portrait), honouring the
// persisted 180-degree flip. Returns false when nothing is pressed.
bool touchRead(int& x, int& y);

void backlight(uint8_t level);   // 0..255 PWM on the PT4103 enable pin

[[noreturn]] void deepSleep();   // wake on BOOT button
[[noreturn]] void rebootToDownloadMode();

ExtensionIOXL9555& expander();
XPowersPPM& charger();
BQ27220& gauge();
TouchDrvGT911& touch();

}  // namespace hw
