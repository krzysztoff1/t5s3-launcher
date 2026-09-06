// t5s3-launcher — factory-partition launcher for the LilyGO T5S3 4.7" e-paper PRO.
//
// Boot decision (docs/boot-flow.md):
//   1. An app asked for the menu (show_menu), a button is held, autostart is
//      off, the last app crash-looped, or we woke from deep sleep -> menu.
//   2. Otherwise autostart: a short countdown (longer on USB, so a host tool can
//      get a command in), then point otadata at the last app and restart.
//   3. No app has run yet -> menu.
#include <Arduino.h>
#include <Wire.h>

#include "board.h"
#include "console.h"
#include "hw.h"
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

static const uint8_t CRASH_LOOP_LIMIT = 3;

static void showMenu() {
    if (!ui::begin()) {
        Serial.println("[launcher] display init failed; console still works (try `help`)");
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    hw::beginI2C();

    const esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("\n[launcher] %s v%s, reset: %s [%d]\n", LAUNCHER_NAME, LAUNCHER_VERSION, resetReasonStr(rr), (int)rr);

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

    const bool menuFlag = registry::consumeShowMenu();
    const bool held = hw::sideButtonPressed() || hw::bootButtonPressed();
    const int last = registry::lastSlot();
    const bool lastValid = last >= 0 && registry::otaPartition(last) != nullptr;
    char notice[96] = {0};

    bool menu = false;
    if (menuFlag)                          { menu = true; }
    else if (held)                         { menu = true; }
    else if (rr == ESP_RST_DEEPSLEEP)      { menu = true; }
    else if (!registry::autostart())       { menu = true; }
    else if (!lastValid)                   { menu = true; }
    else if (crashes >= CRASH_LOOP_LIMIT) {
        menu = true;
        snprintf(notice, sizeof notice, "%s crashed %u times in a row - autostart paused.",
                 registry::slotLabel(last), crashes);
        registry::setCrashes(0);
    }

    if (!menu) {
        // Countdown. Any console byte or a button press turns it into the menu.
        const uint32_t window = hw::usbPowered() ? 2500 : 600;
        Serial.printf("[launcher] autostart %s in %lu ms - press a key or hold a button for the menu\n",
                      registry::slotLabel(last), (unsigned long)window);
        const uint32_t t0 = millis();
        while (millis() - t0 < window) {
            if (Serial.available() || hw::sideButtonPressed() || hw::bootButtonPressed()) { menu = true; break; }
            delay(20);
        }
        if (!menu) {
            registry::boot(last, rr);          // does not return on success
            snprintf(notice, sizeof notice, "Could not start %s.", registry::slotLabel(last));
            menu = true;
        } else {
            Serial.println("[launcher] autostart cancelled");
        }
    }

    if (notice[0]) ui::setNotice(notice);
    else if (isCrash(rr)) {
        snprintf(notice, sizeof notice, "Last reset: %s.", resetReasonStr(rr));
        ui::setNotice(notice);
    }
    showMenu();
}

void loop() {
    console::poll();
    ui::poll();
    delay(10);
}
