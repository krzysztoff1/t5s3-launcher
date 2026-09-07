#include "hwio.h"

#include <Preferences.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include <ExtensionIOXL9555.hpp>
#include <TouchDrvGT911.hpp>
#include <bq27220.h>

#include "board.h"
#include "display.h"

namespace hwio {

static ExtensionIOXL9555 g_io;
static XPowersPPM        g_ppm;
static BQ27220           g_gauge;
static TouchDrvGT911     g_touch;
static bool g_ioOk = false, g_ppmOk = false, g_touchOk = false, g_begun = false;
static bool g_flip = false;
static volatile bool g_homeKey = false;   // set from the GT911 key callback

void begin() {
    if (g_begun) return;
    g_begun = true;
    Wire.begin(BOARD_SDA, BOARD_SCL);
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);

    g_ioOk = g_io.init(Wire, BOARD_SDA, BOARD_SCL, XL9555_SLAVE_ADDRESS0);
    if (g_ioOk) {
        g_io.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);
        g_io.pinMode(IOEXP_PIN_RADIO_POWER, OUTPUT);
        g_io.digitalWrite(IOEXP_PIN_RADIO_POWER, LOW);   // GPS/LoRa rail off: a reader needs neither
    } else {
        Serial.println("[hw] XL9555 expander not found");
    }

    g_ppmOk = g_ppm.init(Wire, BOARD_SDA, BOARD_SCL, BQ25896_SLAVE_ADDRESS);
    if (g_ppmOk) g_ppm.enableMeasure();
    else Serial.println("[hw] BQ25896 charger not found");

    g_gauge.init();   // readings work even when this returns false (see the launcher)

    Preferences p;
    if (p.begin("launcher", true)) {
        g_flip = p.getUChar("tflip", 0) != 0;
        p.end();
    }
}

bool sideButtonPressed() {
    if (!g_ioOk) return false;
    if (g_io.digitalRead(IOEXP_PIN_SIDE_BUTTON) != LOW) return false;
    delayMicroseconds(3000);
    return g_io.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
}

void rearmSideButton() {
    if (g_ioOk) g_io.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);
}

bool bootButtonPressed() { return digitalRead(BOARD_BOOT_BTN) == LOW; }

bool usbPowered() { return g_ppmOk && g_ppm.isVbusIn(); }

int batteryPercent() {
    const uint16_t soc = g_gauge.getStateOfCharge();
    if (soc == 0xFFFF || soc > 100) return -1;
    return (int)soc;
}

bool touchBegin() {
    if (g_touchOk) return true;
    g_touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    g_touchOk = g_touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL);
    if (!g_touchOk) g_touchOk = g_touch.begin(Wire, GT911_SLAVE_ADDRESS_H, BOARD_SDA, BOARD_SCL);
    if (!g_touchOk) Serial.println("[hw] GT911 touch not found");
    // The key below the glass arrives through this callback, which the driver
    // runs from inside getPoint() (same hookup as OpenTrailPaper's).
    if (g_touchOk) g_touch.setHomeButtonCallback([](void*) { g_homeKey = true; }, nullptr);
    // The controller can hold a stale point from before the reset; drain it.
    if (g_touchOk) { int16_t tx, ty; for (int i = 0; i < 3; ++i) g_touch.getPoint(&tx, &ty, 1); }
    g_homeKey = false;   // whatever the drain reported was stale too
    return g_touchOk;
}

bool homeKeyPressed() {
    if (!g_homeKey) return false;
    g_homeKey = false;
    static uint32_t lastAt = 0;
    const uint32_t now = millis();
    if (now - lastAt < 400) return false;   // the flag repeats while the key is held
    lastAt = now;
    return true;
}

bool touchOk() { return g_touchOk; }

bool touchRead(int& x, int& y) {
    if (!g_touchOk) return false;
    int16_t tx = 0, ty = 0;
    if (g_touch.getPoint(&tx, &ty, 1) == 0) return false;
    if (g_flip) { tx = display::W - 1 - tx; ty = display::H - 1 - ty; }
    x = constrain((int)tx, 0, display::W - 1);
    y = constrain((int)ty, 0, display::H - 1);
    return true;
}

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

bool clock(int& hour, int& minute) {
    Wire.beginTransmission(ADDR_RTC);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ADDR_RTC, 7) != 7) return false;
    uint8_t r[7];
    for (int i = 0; i < 7; ++i) r[i] = Wire.read();
    if (r[0] & 0x80) return false;                    // VL bit: clock integrity not guaranteed
    const int year = 2000 + bcd(r[6]);
    if (year < 2024) return false;                    // never set
    hour = bcd(r[2] & 0x3F);
    minute = bcd(r[1] & 0x7F);
    return hour < 24 && minute < 60;
}

static uint8_t toBcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool setClock(int year, int month, int day, int hour, int minute, int second) {
    if (year < 2000 || year > 2099) return false;
    Wire.beginTransmission(ADDR_RTC);
    Wire.write(0x02);
    Wire.write(toBcd(second));
    Wire.write(toBcd(minute));
    Wire.write(toBcd(hour));
    Wire.write(toBcd(day));
    Wire.write(0);                       // weekday: unused
    Wire.write(toBcd(month));            // century bit clear = 20xx
    Wire.write(toBcd(year - 2000));
    return Wire.endTransmission() == 0;
}

void frontLight(uint8_t duty) {
    static bool setup = false;
    if (!setup) {
        ledcSetup(0, 5000, 8);
        ledcAttachPin(BOARD_BL_EN, 0);
        setup = true;
    }
    ledcWrite(0, duty);
}

void deepSleep() {
    Serial.println("[paperback] deep sleep (BOOT wakes)");
    delay(50);   // never Serial.flush() on HWCDC: it hangs without a host
    frontLight(0);
    display::end();
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_BTN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_BTN);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_BTN, 0);
    delay(50);
    esp_deep_sleep_start();
    for (;;) {}
}

void rebootToDownloadMode() {
    Serial.println("[paperback] entering download mode - flash now");
    delay(100);
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    for (;;) {}
}

}  // namespace hwio
