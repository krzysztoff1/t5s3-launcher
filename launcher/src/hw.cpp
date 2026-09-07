#include "hw.h"

#include <Wire.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <soc/rtc_cntl_reg.h>

#include "board.h"
#include "display.h"
#include "registry.h"

namespace hw {

static ExtensionIOXL9555 g_io;
static XPowersPPM        g_ppm;
static BQ27220           g_gauge;
static TouchDrvGT911     g_touch;
static bool g_ioOk = false, g_ppmOk = false, g_gaugeOk = false, g_touchOk = false;
static bool g_i2c = false;
static bool g_flip = false, g_flipLoaded = false;
static volatile bool g_homeKey = false;

ExtensionIOXL9555& expander() { return g_io; }
XPowersPPM& charger() { return g_ppm; }
BQ27220& gauge() { return g_gauge; }
TouchDrvGT911& touch() { return g_touch; }
bool expanderOk() { return g_ioOk; }
bool chargerOk() { return g_ppmOk; }
bool gaugeOk() { return g_gaugeOk; }
bool touchOk() { return g_touchOk; }

void beginI2C() {
    if (g_i2c) return;
    g_i2c = true;
    Wire.begin(BOARD_SDA, BOARD_SCL);
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);

    g_ioOk = g_io.init(Wire, BOARD_SDA, BOARD_SCL, XL9555_SLAVE_ADDRESS0);
    if (g_ioOk) {
        g_io.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);
        g_io.pinMode(IOEXP_PIN_RADIO_POWER, OUTPUT);
        g_io.digitalWrite(IOEXP_PIN_RADIO_POWER, LOW);   // radios off until asked
    } else {
        Serial.println("[hw] XL9555 expander not found");
    }

    g_ppmOk = g_ppm.init(Wire, BOARD_SDA, BOARD_SCL, BQ25896_SLAVE_ADDRESS);
    if (g_ppmOk) {
        g_ppm.enableMeasure();     // ADC on, or every voltage reads 0
    } else {
        Serial.println("[hw] BQ25896 charger not found");
    }

    // The gauge's init() also programs its data memory; the readings work
    // without it, so a false here is a warning, not a failure (OpenTrailPaper
    // sees the same).
    g_gaugeOk = g_gauge.init();
}

bool sideButtonPressed() {
    if (!g_ioOk) return false;
    // Two reads a few ms apart, as OpenTrailPaper does: a single LOW can be an
    // I2C collision with the display driver's power-control traffic.
    if (g_io.digitalRead(IOEXP_PIN_SIDE_BUTTON) != LOW) return false;
    delayMicroseconds(3000);
    return g_io.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
}

void rearmSideButton() {
    // epd_painter_powerctl::begin() does pcaPinMode(8..13, OUTPUT); pin 10 is
    // not the driver's, it is the button, and driven low it reads "pressed"
    // forever. XL9555::pinMode() read-modify-writes, so only bit 10 changes.
    if (g_ioOk) g_io.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);
}

bool bootButtonPressed() {
    return digitalRead(BOARD_BOOT_BTN) == LOW;
}

void radioPower(bool on) {
    if (!g_ioOk) return;
    g_io.digitalWrite(IOEXP_PIN_RADIO_POWER, on ? HIGH : LOW);
}

bool usbPowered() {
    return g_ppmOk && g_ppm.isVbusIn();
}

int batteryPercent() {
    uint16_t soc = g_gauge.getStateOfCharge();
    if (soc == 0xFFFF || soc > 100) return -1;
    return (int)soc;
}

uint16_t batteryMillivolts() {
    uint16_t mv = g_gauge.getVoltage();
    return (mv == 0xFFFF) ? 0 : mv;
}

bool touchBegin() {
    if (g_touchOk) return true;
    g_touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    g_touchOk = g_touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL);
    if (!g_touchOk) g_touchOk = g_touch.begin(Wire, GT911_SLAVE_ADDRESS_H, BOARD_SDA, BOARD_SCL);
    if (!g_touchOk) { Serial.println("[hw] GT911 touch not found"); return false; }
    // The key below the glass arrives as a callback from inside getPoint().
    g_touch.setHomeButtonCallback([](void*) { g_homeKey = true; }, nullptr);
    // The controller can hold a stale point from before the reset; drain it.
    int16_t tx, ty;
    for (int i = 0; i < 3; ++i) g_touch.getPoint(&tx, &ty, 1);
    return true;
}

void setTouchFlip(bool on) { g_flip = on; g_flipLoaded = true; }

bool homeKeyPressed() {
    if (!g_homeKey) return false;
    g_homeKey = false;
    // The callback fires on every poll while the key is held: one press, one event.
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last < 400) return false;
    last = now;
    return true;
}

bool touchRead(int& x, int& y) {
    if (!g_touchOk) return false;
    int16_t tx = 0, ty = 0;
    if (g_touch.getPoint(&tx, &ty, 1) == 0) return false;
    if (!g_flipLoaded) setTouchFlip(registry::touchFlip());
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
    if (2000 + bcd(r[6]) < 2024) return false;        // never set
    hour = bcd(r[2] & 0x3F);
    minute = bcd(r[1] & 0x7F);
    return hour < 24 && minute < 60;
}

void backlight(uint8_t level) {
    static bool setup = false;
    if (!setup) {
        ledcSetup(0, 5000, 8);
        ledcAttachPin(BOARD_BL_EN, 0);
        setup = true;
    }
    ledcWrite(0, level);
}

void deepSleep() {
    Serial.println("[launcher] deep sleep (BOOT button wakes)");
    delay(50);   // never Serial.flush() on HWCDC: it hangs without a host
    backlight(0);
    radioPower(false);
    display::end();
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_BTN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_BTN);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_BTN, 0);
    delay(50);
    esp_deep_sleep_start();
    for (;;) {}
}

void rebootToDownloadMode() {
    // What usb_persist_restart(RESTART_BOOTLOADER) does underneath: a sticky RTC
    // bit the ROM honours on the next reset. tools/flash.py clears it again.
    Serial.println("[launcher] entering download mode - flash now");
    delay(100);  // no flush, see deepSleep()
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    for (;;) {}
}

}  // namespace hw
