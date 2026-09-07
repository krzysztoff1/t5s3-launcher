// Paperback — an e-book reader for the LilyGO T5S3 4.7" e-paper PRO, running
// under t5s3-launcher. Plain-text books (embedded samples and /books/*.txt on
// the SD card) laid out with real typography; Hacker News over Wi-Fi.
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "app.h"
#include "console.h"
#include "display.h"
#include "font.h"
#include "hn.h"
#include "hwio.h"
#include "launcher_api.h"
#include "library.h"
#include "settings.h"

void setup() {
    launcher::handoff(APP_NAME, APP_VERSION);   // must stay the first line

    Serial.begin(115200);
    delay(50);
    const esp_reset_reason_t rr = launcher::previousResetReason();
    const bool wake = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    Serial.printf("\n[paperback] %s %s starting (reset %d%s)\n", APP_NAME, APP_VERSION, (int)rr, wake ? ", deep-sleep wake" : "");

    hwio::begin();
    settings::load();
    if (!font::begin()) Serial.println("[paperback] font init incomplete");
    hwio::touchBegin();
    library::scan();

    // Credentials on the card win over stored ones, so a fresh card can re-point the device.
    {
        char ssid[33], pass[65];
        if (library::sdWifi(ssid, sizeof ssid, pass, sizeof pass)) {
            Settings& s = settings::get();
            if (strcmp(s.wifiSsid, ssid) != 0 || strcmp(s.wifiPass, pass) != 0) {
                strlcpy(s.wifiSsid, ssid, sizeof s.wifiSsid);
                strlcpy(s.wifiPass, pass, sizeof s.wifiPass);
                settings::save();
                Serial.printf("[paperback] wifi credentials taken from the SD card (%s)\n", ssid);
            }
        }
    }
    hwio::frontLight(settings::lightDuty());

    app::begin(wake);
    Serial.printf("[paperback] running; internal heap %u, PSRAM %u. Type `help` for commands.\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
    console::poll();
    app::poll();
    delay(8);
}
