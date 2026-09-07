// t5s3-launcher — factory-partition launcher for the LilyGO T5S3 4.7" e-paper PRO.
//
// Boot (docs/boot-flow.md): the launcher always shows its menu. There is no
// autostart — an app starts only when the user taps it on screen or a host runs
// `flash.py boot`. A deep-sleep wake fast-boots the running app before the
// launcher is even reached, so the menu is not involved there.
#include <Arduino.h>
#include <Wire.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "board.h"
#include "console.h"
#include "hw.h"
#include "launcher_api.h"
#include "registry.h"
#include "ui.h"
#include "version.h"

static const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "RST button";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "brownout";
        default:                return "unknown";
    }
}

static bool isCrash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}


// An app running USB in OTG mode (OpenTrailPaper) hands the S3's single USB PHY
// to the OTG controller through RTC-domain bits that survive esp_restart().
// After such an app hands back, the launcher would come up with USB-Serial-JTAG
// but no PHY: alive, drawing the menu, invisible to the host until a physical
// RESET. Take the PHY back before Serial starts. Same bits the Arduino core
// clears in usb_switch_to_cdc_jtag() (esp32-hal-tinyusb.c).
static bool reclaimUsbPhy() {
    if (!REG_GET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL)) return false;
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    return true;
}

static void showMenu() {
    if (!ui::begin()) {
        Serial.println("[launcher] display init failed; console still works (try `help`)");
    }
}

void setup() {
    const bool reclaimed = reclaimUsbPhy();
    Serial.begin(115200);
    delay(50);
    hw::beginI2C();
    hw::backlight(launcher::frontLight());   // one front-light setting for the launcher and every app

    const esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("\n[launcher] %s v%s, reset: %s [%d]\n", LAUNCHER_NAME, LAUNCHER_VERSION, resetReasonStr(rr), (int)rr);
    if (reclaimed) Serial.println("[launcher] took the USB PHY back from an OTG-mode app");

    // Crash accounting: `armed` was set right before we last started an app, so
    // a crash reset now means that app crashed.
    const bool armed = registry::consumeArmed();
    uint8_t crashes = registry::crashes();
    if (armed && isCrash(rr)) {
        crashes++;
        registry::setCrashes(crashes);
        Serial.printf("[launcher] the last app ended in a %s (%u in a row)\n", resetReasonStr(rr), crashes);
    } else if (!isCrash(rr) && rr != ESP_RST_SW) {
        if (crashes) registry::setCrashes(0);
        crashes = 0;
    }

    // No autostart: the launcher always shows its menu. An app starts only when
    // the user taps it (or a host runs `flash.py boot`), so the menu is always
    // one reset away and the device never launches an app on its own.
    registry::consumeShowMenu();               // clear any returnToLauncher() flag

    char notice[96] = {0};
    if (armed && isCrash(rr)) {
        const int last = registry::lastSlot();
        snprintf(notice, sizeof notice, "%s ended in a %s.",
                 last >= 0 ? registry::slotLabel(last) : "The last app", resetReasonStr(rr));
    } else if (isCrash(rr)) {
        snprintf(notice, sizeof notice, "Last reset: %s.", resetReasonStr(rr));
    }
    if (notice[0]) ui::setNotice(notice);
    showMenu();
}

void loop() {
    console::poll();
    ui::poll();
    delay(10);
}
