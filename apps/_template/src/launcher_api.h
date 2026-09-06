// launcher_api.h — the one header an app includes to live alongside t5s3-launcher.
//
// Header-only, Arduino-ESP32 2.x (ESP-IDF 4.4). Apps carry a verbatim copy of
// this file (tools/check-sync.sh keeps the copies identical), so keep it
// dependency-free beyond the Arduino core.
//
// Contract, in two calls:
//
//   void setup() {
//       launcher::handoff("MyApp", "v1.0");   // FIRST line of setup()
//       ...
//   }
//
//   // From a menu entry / console command / long press:
//   launcher::returnToLauncher();
//
// How it works (details in docs/boot-flow.md):
//
//   * The launcher lives in the `factory` partition, apps in ota_0/ota_1. The
//     launcher starts an app by pointing otadata at its slot and restarting.
//   * handoff() records which slot is running (so the launcher can label and
//     autostart it) and ERASES otadata, so the next cold boot — power-on, RESET
//     button, crash, esp_restart() — lands in the launcher again. The launcher
//     then autostarts the last app unless a button is held or the app has been
//     crash-looping. This is what makes "hold the front button through a reset"
//     always reach the launcher, with no bootloader changes.
//   * Wakes from deep sleep bypass all of this: the stock bootloader fast-boots
//     the last image straight from RTC memory, so an app's sleep/wake cycle is
//     unaffected by the otadata erase.
//   * Because the launcher reaches an app via esp_restart(), the app's own
//     esp_reset_reason() reads ESP_RST_SW. previousResetReason() gives the real
//     reason of the reset that preceded the launcher hop (e.g. ESP_RST_PANIC),
//     for crash-recovery logic that keys off it.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string.h>
#include <stdio.h>
// Same test the Arduino core uses: OTG builds leave ARDUINO_USB_MODE undefined or 0.
#if defined(CONFIG_IDF_TARGET_ESP32S3) && !ARDUINO_USB_MODE
#include <driver/periph_ctrl.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>
#define LAUNCHER_API_OTG_USB 1
#endif

namespace launcher {

// NVS namespace shared by the launcher and every app. Keys (NVS limit: 15 chars):
//   last_slot  u8   OTA index (0 = ota_0) of the app that last ran
//   autostart  u8   1 = cold boot goes straight to last_slot (default 1)
//   show_menu  u8   1 = an app asked for the menu; the launcher consumes it
//   app_rr     u8   reset reason preceding the launcher's last app start
//   crashes    u8   consecutive autostarts that ended in a crash (launcher-owned)
//   n<i>       str  display name registered for OTA slot i
//   v<i>       str  version string registered for OTA slot i
static constexpr const char* kNvsNamespace = "launcher";

// OTA index of an app partition (0 for ota_0), or -1 for factory / non-app.
inline int slotIndex(const esp_partition_t* p) {
    if (!p || p->type != ESP_PARTITION_TYPE_APP) return -1;
    if (p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        p->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return -1;
    return (int)p->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
}

// Erase otadata so the bootloader's next normal boot selects `factory`. This is
// exactly what esp_ota_set_boot_partition(factory) does, minus a full
// re-validation of the factory image we do not need. Skips the erase when the
// partition is already blank, so calling it on every boot costs a 64-byte read.
inline void eraseOtadata() {
    const esp_partition_t* od = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (!od) return;
    bool blank = true;
    uint8_t buf[32];
    for (int sector = 0; sector < 2 && blank; ++sector) {
        if (esp_partition_read(od, (size_t)sector * 0x1000, buf, sizeof buf) != ESP_OK) {
            blank = false;
            break;
        }
        for (uint8_t b : buf) if (b != 0xFF) { blank = false; break; }
    }
    if (!blank) esp_partition_erase_range(od, 0, od->size);
}

namespace detail {
inline esp_reset_reason_t& storedResetReason() {
    static esp_reset_reason_t rr = ESP_RST_UNKNOWN;
    return rr;
}
inline bool& handoffDone() {
    static bool done = false;
    return done;
}
}  // namespace detail

// Call as the FIRST line of setup(). Safe to call when not running under the
// launcher (e.g. an image flashed straight into the factory slot): it then
// does nothing.
inline void handoff(const char* name, const char* version) {
    if (detail::handoffDone()) return;
    detail::handoffDone() = true;
    detail::storedResetReason() = esp_reset_reason();

    const int idx = slotIndex(esp_ota_get_running_partition());
    if (idx < 0) return;

    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        char key[8];
        snprintf(key, sizeof key, "n%d", idx);
        p.putString(key, name ? name : "");
        snprintf(key, sizeof key, "v%d", idx);
        p.putString(key, version ? version : "");
        p.putUChar("last_slot", (uint8_t)idx);
        // The launcher stores the true reset reason right before it starts us;
        // consume it so a later self-restart cannot replay a stale crash.
        if (p.isKey("app_rr")) {
            uint8_t rr = p.getUChar("app_rr", 0xFF);
            if (rr != 0xFF && esp_reset_reason() == ESP_RST_SW)
                detail::storedResetReason() = (esp_reset_reason_t)rr;
            p.remove("app_rr");
        }
        p.end();
    }
    eraseOtadata();
}

// The reset reason that actually ended the previous session. Identical to
// esp_reset_reason() unless the launcher hop is in the way. Valid after
// handoff(); before it, falls back to esp_reset_reason().
inline esp_reset_reason_t previousResetReason() {
    return detail::handoffDone() ? detail::storedResetReason() : esp_reset_reason();
}

// Flag the launcher to show its menu (no autostart) and reboot into it now.
[[noreturn]] inline void returnToLauncher() {
    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        p.putUChar("show_menu", 1);
        p.end();
    }
    eraseOtadata();
    delay(50);
#ifdef LAUNCHER_API_OTG_USB
    // This app runs USB in OTG mode. The PHY selection lives in RTC registers
    // that survive esp_restart(); give the PHY back to USB-Serial-JTAG so the
    // launcher is reachable over USB (the launcher also does this itself, this
    // is belt and braces). Mirrors usb_switch_to_cdc_jtag() in the Arduino core.
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
#endif
    esp_restart();
    for (;;) {}
}

}  // namespace launcher
