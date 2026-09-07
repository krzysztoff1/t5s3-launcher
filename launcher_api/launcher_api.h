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
//   light      u8   front light PWM duty 0..255, shared by everyone (see frontLight())
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

// Front light, shared by the launcher and every app so the level set in one
// place is the level everywhere: the PT4103's PWM duty (0 = off .. 255), NVS
// key "light" in the shared namespace. Apps map it to their own steps.
inline bool hasFrontLight() {
    Preferences p;
    bool has = false;
    if (p.begin(kNvsNamespace, true)) { has = p.isKey("light"); p.end(); }
    return has;
}
inline uint8_t frontLight() {
    Preferences p;
    uint8_t duty = 0;
    if (p.begin(kNvsNamespace, true)) { duty = p.getUChar("light", 0); p.end(); }
    return duty;
}
inline void setFrontLight(uint8_t duty) {
    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        if (p.getUChar("light", 0xFF) != duty) p.putUChar("light", duty);
        p.end();
    }
}

// Screenshot. The panel is four-level greyscale and every firmware here keeps
// its framebuffer as one byte per pixel, 0 (paper white) .. 3 (ink black), so
// one dumper serves the launcher and every app: run-length encode the pixels,
// base64 the tokens, print them between two markers. `tools/flash.py
// screenshot` turns that back into a PNG of exactly what is on the glass.
// Streams straight into Serial, so it costs ~100 bytes of stack and no heap.
//
//   [screenshot] begin <w> <h> 4
//   <base64, 76 characters per line>
//   [screenshot] end <tokenBytes> <crc32>
//
// A token is (level << 6) | k. k <= 62 is a run of k + 1 pixels; k == 63 means
// two more bytes follow, big-endian, and the run is 63 + those.
namespace detail {

inline uint32_t crc32Byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    return crc;
}

// Base64 into fixed-width lines, one byte at a time.
struct ScreenDumper {
    char     line[80];
    uint8_t  trio[3];
    int      lineLen = 0;
    int      trioLen = 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t count = 0;

    void putChar(char c) {
        line[lineLen++] = c;
        if (lineLen == 76) {
            line[lineLen] = 0;
            Serial.println(line);
            lineLen = 0;
            yield();   // thousands of lines go past; let the rest of the system breathe
        }
    }
    void putTrio(int n) {
        static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint32_t v = ((uint32_t)trio[0] << 16) |
                           ((uint32_t)(n > 1 ? trio[1] : 0) << 8) |
                           (uint32_t)(n > 2 ? trio[2] : 0);
        putChar(kB64[(v >> 18) & 63]);
        putChar(kB64[(v >> 12) & 63]);
        putChar(n > 1 ? kB64[(v >> 6) & 63] : '=');
        putChar(n > 2 ? kB64[v & 63] : '=');
    }
    void putByte(uint8_t b) {
        crc = crc32Byte(crc, b);
        count++;
        trio[trioLen++] = b;
        if (trioLen == 3) { putTrio(3); trioLen = 0; }
    }
    void finish() {
        if (trioLen) { putTrio(trioLen); trioLen = 0; }
        if (lineLen) { line[lineLen] = 0; Serial.println(line); lineLen = 0; }
    }
};

}  // namespace detail

inline void dumpScreen(const uint8_t* fb, int w, int h) {
    if (!fb || w <= 0 || h <= 0) {
        Serial.println("[screenshot] no framebuffer");
        return;
    }
    Serial.printf("[screenshot] begin %d %d 4\n", w, h);
    detail::ScreenDumper d;
    const size_t n = (size_t)w * (size_t)h;
    const size_t kMaxRun = 63 + 65535;
    size_t i = 0;
    while (i < n) {
        const uint8_t v = fb[i] & 3;
        size_t run = 1;
        while (i + run < n && (fb[i + run] & 3) == v && run < kMaxRun) run++;
        i += run;
        if (run <= 63) {
            d.putByte((uint8_t)((v << 6) | (uint8_t)(run - 1)));
        } else {
            const uint32_t extra = (uint32_t)(run - 63);
            d.putByte((uint8_t)((v << 6) | 63));
            d.putByte((uint8_t)(extra >> 8));
            d.putByte((uint8_t)(extra & 0xFF));
        }
    }
    d.finish();
    Serial.printf("[screenshot] end %u %08X\n", (unsigned)d.count, (unsigned)(d.crc ^ 0xFFFFFFFFu));
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
