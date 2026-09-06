#include "syscheck.h"

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <SensorPCF8563.hpp>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_idf_version.h>
#include <stdarg.h>

#include "board.h"
#include "display.h"
#include "hw.h"
#include "registry.h"
#include "version.h"

using namespace display;

namespace syscheck {

struct Line { char name[12]; char status[5]; char detail[128]; };
static Line g_lines[24];
static int  g_n = 0;
static int  g_pass = 0, g_warn = 0, g_fail = 0;
static char g_hint[96] = {0};

static void redraw() {
    if (!display::ready()) return;
    GFXcanvas8& g = gfx();
    g.fillScreen(WHITE);
    g.fillRect(0, 0, W, 72, BLACK);
    text(24, 50, "System check", FONT_TITLE, WHITE);
    int y = 112;
    for (int i = 0; i < g_n; ++i) {
        const Line& l = g_lines[i];
        const bool bad = strcmp(l.status, "FAIL") == 0;
        const bool warn = strcmp(l.status, "WARN") == 0;
        if (bad) g.fillRect(12, y - 22, W - 24, 30, LIGHT);
        text(20, y, l.name, FONT_ITEM, BLACK);
        text(160, y, l.status, FONT_ITEM, bad ? BLACK : (warn ? DARK : BLACK));
        textFit(236, y, l.detail, FONT_SMALL, W - 250, BLACK);
        y += 34;
    }
    if (g_hint[0]) {
        g.fillRect(12, H - 120, W - 24, 60, LIGHT);
        textCentered(W / 2, H - 82, g_hint, FONT_BODY, BLACK);
    }
    paint();
}

static void hint(const char* h) {
    strlcpy(g_hint, h ? h : "", sizeof g_hint);
    redraw();
}

static void report(const char* name, const char* status, const char* fmt, ...) {
    Line& l = (g_n < (int)(sizeof g_lines / sizeof g_lines[0])) ? g_lines[g_n++] : g_lines[g_n - 1];
    strlcpy(l.name, name, sizeof l.name);
    strlcpy(l.status, status, sizeof l.status);
    va_list ap; va_start(ap, fmt);
    vsnprintf(l.detail, sizeof l.detail, fmt, ap);
    va_end(ap);
    if (!strcmp(status, "PASS")) g_pass++; else if (!strcmp(status, "WARN")) g_warn++; else g_fail++;
    Serial.printf("[syscheck] %s: %s - %s\n", name, status, l.detail);
    g_hint[0] = 0;
    redraw();
}

// --- steps -------------------------------------------------------------------
static void checkChip() {
    esp_chip_info_t ci; esp_chip_info(&ci);
    report("chip", "PASS", "ESP32-S3 rev%d %dcore %dMHz flash %uMB idf %s heap %uK",
           ci.revision, ci.cores, (int)getCpuFrequencyMhz(),
           (unsigned)(ESP.getFlashChipSize() >> 20), esp_get_idf_version(),
           (unsigned)(ESP.getFreeHeap() >> 10));
}

static void checkPsram() {
    const size_t total = ESP.getPsramSize();
    if (total == 0) { report("psram", "FAIL", "no PSRAM detected"); return; }
    const size_t n = 1024 * 1024;
    uint32_t* buf = (uint32_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf) { report("psram", "FAIL", "%uMB reported, 1MB alloc failed", (unsigned)(total >> 20)); return; }
    uint32_t t0 = micros();
    for (size_t i = 0; i < n / 4; ++i) buf[i] = (uint32_t)i * 2654435761u;
    size_t bad = 0;
    for (size_t i = 0; i < n / 4; ++i) if (buf[i] != (uint32_t)i * 2654435761u) bad++;
    uint32_t dt = micros() - t0;
    heap_caps_free(buf);
    report("psram", bad ? "FAIL" : "PASS", "%uMB, %u free, 1MB r/w %s in %lums",
           (unsigned)(total >> 20), (unsigned)(ESP.getFreePsram() >> 10) , bad ? "MISMATCH" : "ok",
           (unsigned long)(dt / 1000));
}

static void checkPartitions() {
    registry::Slot slots[8];
    const int n = registry::scan(slots, 8);
    char buf[160]; int o = 0;
    bool runningFactory = false;
    for (int i = 0; i < n && o < (int)sizeof buf - 24; ++i) {
        const registry::Slot& s = slots[i];
        if (s.index < 0 && s.running) runningFactory = true;
        o += snprintf(buf + o, sizeof buf - o, "%s%s=%s%s", i ? " " : "", registry::slotLabel(s.index),
                      s.valid ? (s.index < 0 ? "launcher" : s.name) : "empty", s.running ? "*" : "");
    }
    report("slots", runningFactory ? "PASS" : "WARN", "%s%s", buf,
           runningFactory ? "" : " (launcher not in factory slot!)");
}

static void checkI2C() {
    struct Known { uint8_t addr; const char* name; bool required; };
    static const Known known[] = {
        {ADDR_IOEXP, "XL9555", true}, {ADDR_RTC, "PCF8563", true}, {ADDR_GAUGE, "BQ27220", true},
        {ADDR_TOUCH_L, "GT911", false}, {ADDR_TOUCH_H, "GT911", false},
        {ADDR_TPS65185, "TPS65185", true}, {ADDR_CHARGER, "BQ25896", true},
    };
    char found[160]; int o = 0; int nfound = 0;
    bool seen[128] = {false};
    for (uint8_t a = 0x08; a < 0x78; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            seen[a] = true; nfound++;
            const char* nm = "?";
            for (const Known& k : known) if (k.addr == a) nm = k.name;
            if (o < (int)sizeof found - 16) o += snprintf(found + o, sizeof found - o, "%s0x%02X:%s", o ? " " : "", a, nm);
        }
    }
    char missing[80] = {0}; int m = 0;
    for (const Known& k : known) {
        if (k.required && !seen[k.addr]) m += snprintf(missing + m, sizeof missing - m, " %s", k.name);
    }
    if (!seen[ADDR_TOUCH_L] && !seen[ADDR_TOUCH_H]) m += snprintf(missing + m, sizeof missing - m, " GT911");
    report("i2c", missing[0] ? "WARN" : "PASS", "%d devices: %s%s%s", nfound, found,
           missing[0] ? " MISSING:" : "", missing);
}

static void checkExpander() {
    if (!hw::expanderOk()) { report("expander", "FAIL", "XL9555 init failed"); return; }
    report("expander", "PASS", "XL9555 ok, side button %s, radio rail %s",
           hw::sideButtonPressed() ? "PRESSED" : "released",
           hw::expander().digitalRead(IOEXP_PIN_RADIO_POWER) ? "on" : "off");
}

static void checkRtc() {
    SensorPCF8563 rtc;
    if (!rtc.init(Wire, BOARD_SDA, BOARD_SCL)) { report("rtc", "FAIL", "PCF8563 init failed"); return; }
    RTC_DateTime dt = rtc.getDateTime();
    if (dt.year < 2024 || dt.year > 2099) {
        RTC_DateTime build(__DATE__, __TIME__);
        rtc.setDateTime(build);
        RTC_DateTime now = rtc.getDateTime();
        report("rtc", "WARN", "time was invalid (%04d-%02d-%02d); set to build time %04d-%02d-%02d %02d:%02d",
               dt.year, dt.month, dt.day, now.year, now.month, now.day, now.hour, now.minute);
        return;
    }
    report("rtc", "PASS", "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

static void checkGauge() {
    BQ27220& g = hw::gauge();
    const uint16_t dev = g.getDeviceNumber();
    if (dev != 0x0220) { report("gauge", "FAIL", "BQ27220 not answering (device 0x%04X)", dev); return; }
    const uint16_t mv = g.getVoltage(); const int16_t ma = g.getCurrent();
    const uint16_t soc = g.getStateOfCharge(); const uint16_t traw = g.getTemperature();
    const float tc = traw ? traw / 10.0f - 273.15f : 0;
    report("gauge", hw::gaugeOk() ? "PASS" : "WARN", "%umV %dmA soc %u%% %.1fC cap %u/%umAh soh %u%%%s",
           mv, ma, soc, tc, g.getRemainingCapacity(), g.getFullChargeCapacity(), g.getStateOfHealth(),
           hw::gaugeOk() ? "" : " (init() false, readings raw)");
}

static void checkCharger() {
    if (!hw::chargerOk()) { report("charger", "FAIL", "BQ25896 init failed"); return; }
    XPowersPPM& p = hw::charger();
    report("charger", "PASS", "vbus %s %umV, batt %umV, sys %umV, %s, %umA",
           p.isVbusIn() ? "IN" : "none", p.getVbusVoltage(), p.getBattVoltage(), p.getSystemVoltage(),
           p.getChargeStatusString(), p.getChargeCurrent());
}

static void checkTouch(bool interactive) {
    if (!hw::touchBegin()) { report("touch", "FAIL", "GT911 not found at 0x5D/0x14"); return; }
    int16_t rx = 0, ry = 0;
    hw::touch().getResolution(&rx, &ry);
    if (!interactive) { report("touch", "PASS", "GT911 ok, resolution %dx%d (no touch test)", rx, ry); return; }
    hint("TOUCH THE SCREEN (8 s)");
    const uint32_t t0 = millis();
    int touches = 0, lx = -1, ly = -1; uint32_t lastPaint = 0;
    while (millis() - t0 < 8000) {
        int x, y;
        if (hw::touchRead(x, y)) {
            touches++; lx = x; ly = y;
            GFXcanvas8& g = gfx();
            g.drawCircle(x, y, 18, BLACK); g.drawCircle(x, y, 19, BLACK);
            g.drawLine(x - 30, y, x + 30, y, BLACK); g.drawLine(x, y - 30, x, y + 30, BLACK);
            if (millis() - lastPaint > 700) { paint(); lastPaint = millis(); }
            uint32_t t1 = millis();
            while (hw::touchRead(x, y) && millis() - t1 < 1500) delay(10);
        }
        delay(10);
    }
    if (touches) report("touch", "PASS", "GT911 %dx%d, %d touches, last at %d,%d%s (flip %s)", rx, ry, touches, lx, ly,
                        "", registry::touchFlip() ? "on" : "off");
    else report("touch", "WARN", "GT911 %dx%d ok, nobody touched it", rx, ry);
}

static bool g_spi = false;
static void spiBegin() {
    if (g_spi) return;
    pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);   digitalWrite(BOARD_SD_CS, HIGH);
    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI, BOARD_SD_CS);
    g_spi = true;
}

static void checkSd() {
    spiBegin();
    if (!SD.begin(BOARD_SD_CS, SPI, 20000000)) { report("sd", "WARN", "no card / mount failed"); return; }
    const uint8_t type = SD.cardType();
    const char* tname = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC" : type == CARD_SDHC ? "SDHC" : "?";
    const uint64_t sizeMB = SD.cardSize() / (1024 * 1024);
    const uint64_t usedMB = SD.usedBytes() / (1024 * 1024);
    // 256 KB write + read-back.
    const size_t chunk = 4096, total = 256 * 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    bool ok = buf != nullptr; float wKB = 0, rKB = 0;
    if (ok) {
        for (size_t i = 0; i < chunk; ++i) buf[i] = (uint8_t)(i * 7 + 3);
        File f = SD.open("/syscheck.tmp", FILE_WRITE);
        ok = (bool)f;
        uint32_t t0 = micros();
        for (size_t w = 0; ok && w < total; w += chunk) ok = f.write(buf, chunk) == chunk;
        if (f) f.close();
        uint32_t dt = micros() - t0; wKB = dt ? (total / 1024.0f) / (dt / 1e6f) : 0;
        if (ok) {
            f = SD.open("/syscheck.tmp", FILE_READ);
            ok = (bool)f;
            t0 = micros();
            for (size_t r = 0; ok && r < total; r += chunk) {
                ok = f.read(buf, chunk) == chunk;
                for (size_t i = 0; ok && i < chunk; i += 97) ok = buf[i] == (uint8_t)(i * 7 + 3);
            }
            if (f) f.close();
            dt = micros() - t0; rKB = dt ? (total / 1024.0f) / (dt / 1e6f) : 0;
        }
        SD.remove("/syscheck.tmp");
        heap_caps_free(buf);
    }
    report("sd", ok ? "PASS" : "FAIL", "%s %lluMB, %lluMB used, write %.0f KB/s read %.0f KB/s%s",
           tname, (unsigned long long)sizeMB, (unsigned long long)usedMB, wKB, rKB, ok ? "" : " (r/w test failed)");
    SD.end();
}

static bool nmeaValid(const char* s) {
    if (s[0] != '$') return false;
    const char* star = strchr(s, '*');
    if (!star || strlen(star) < 3) return false;
    uint8_t cs = 0;
    for (const char* p = s + 1; p < star; ++p) cs ^= (uint8_t)*p;
    return (uint8_t)strtol(star + 1, nullptr, 16) == cs;
}

static int gpsListen(uint32_t baud, uint32_t ms, int& sats, int& fix, char* sample, size_t sampleLen) {
    Serial1.begin(baud, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    Serial1.setTimeout(20);
    const uint32_t t0 = millis();
    int valid = 0;
    char line[128]; size_t n = 0;
    while (millis() - t0 < ms) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '\n' || c == '\r') {
                if (n) {
                    line[n] = 0;
                    if (nmeaValid(line)) {
                        valid++;
                        if (!sample[0]) strlcpy(sample, line, sampleLen);
                        if (strstr(line, "GGA,")) {
                            // $xxGGA,time,lat,N,lon,E,fix,sats,...
                            int field = 0; const char* p = line;
                            while (*p && field < 7) { if (*p == ',') field++; p++; if (field == 6 && *(p) != ',' && fix < 0) fix = atoi(p); }
                            if (field == 7) sats = atoi(p);
                        }
                    }
                    n = 0;
                }
            } else if (n < sizeof line - 1) line[n++] = c;
        }
        delay(5);
    }
    Serial1.end();
    return valid;
}

static void checkGps() {
    if (!hw::expanderOk()) { report("gps", "FAIL", "cannot power the radio rail (no expander)"); return; }
    hw::radioPower(true);
    delay(600);
    hint("Listening to the GPS...");
    int sats = -1, fix = -1; char sample[64] = {0};
    int valid = gpsListen(38400, 2500, sats, fix, sample, sizeof sample);
    uint32_t baud = 38400;
    if (!valid) { baud = 9600; valid = gpsListen(9600, 2500, sats, fix, sample, sizeof sample); }
    if (!valid) { report("gps", "FAIL", "no NMEA at 38400 or 9600 (rail on, RX44/TX43)"); return; }
    char talker[8] = {0};
    strncpy(talker, sample + 1, 5);
    report("gps", "PASS", "%d sentences at %lu baud (%s), sats %d, fix %d", valid, (unsigned long)baud, talker,
           sats, fix);
}

static void checkLora() {
    spiBegin();
    static SX1262 radio = new Module(BOARD_LORA_CS, BOARD_LORA_IRQ, BOARD_LORA_RST, BOARD_LORA_BUSY);
    hw::radioPower(true);
    delay(200);
    int st = radio.begin(868.0, 125.0, 9, 7, 0x12, 10, 8, BOARD_LORA_TCXO_V, false);
    if (st == RADIOLIB_ERR_NONE) {
        radio.setDio2AsRfSwitch(true);
        radio.sleep();
        report("lora", "PASS", "SX1262 answered (868 MHz test config, TCXO %.1fV)", BOARD_LORA_TCXO_V);
    } else {
        report("lora", "FAIL", "SX1262 begin() error %d", st);
    }
    hw::radioPower(false);
}

static void checkWifi() {
    hint("Scanning Wi-Fi...");
    const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    WiFi.mode(WIFI_STA);
    delay(100);
    int n = WiFi.scanNetworks(false, false, false, 300);
    if (n < 0) { report("wifi", "FAIL", "scan error %d (internal heap %u)", n, (unsigned)before); WiFi.mode(WIFI_OFF); return; }
    int best = -1;
    for (int i = 0; i < n; ++i) if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    report("wifi", "PASS", "%d networks%s%s %ddBm", n, best >= 0 ? ", strongest " : "",
           best >= 0 ? WiFi.SSID(best).c_str() : "", best >= 0 ? WiFi.RSSI(best) : 0);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

static void checkBle() {
    hint("Scanning BLE...");
    const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (!NimBLEDevice::init(LAUNCHER_NAME)) { report("ble", "FAIL", "NimBLE init failed (internal heap %u)", (unsigned)before); return; }
    NimBLEScan* sc = NimBLEDevice::getScan();
    sc->setActiveScan(false);
    NimBLEScanResults res = sc->getResults(3000, false);
    const int n = res.getCount();
    sc->clearResults();
    NimBLEDevice::deinit(true);
    report("ble", "PASS", "%d devices seen in 3 s, addr %s", n, NimBLEDevice::getAddress().toString().c_str());
}

static void checkButtons(bool interactive) {
    if (!interactive) { report("buttons", "PASS", "BOOT %s, side %s (no press test)",
                               hw::bootButtonPressed() ? "down" : "up", hw::sideButtonPressed() ? "down" : "up"); return; }
    hint("PRESS BOOT, THEN THE SIDE BUTTON (6 s)");
    bool sawBoot = false, sawSide = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < 6000 && !(sawBoot && sawSide)) {
        if (hw::bootButtonPressed()) sawBoot = true;
        if (hw::sideButtonPressed()) sawSide = true;
        delay(10);
    }
    report("buttons", (sawBoot && sawSide) ? "PASS" : "WARN", "BOOT %s, side %s",
           sawBoot ? "pressed" : "not seen", sawSide ? "pressed" : "not seen");
}

static void checkBacklight() {
    for (int v = 0; v <= 255; v += 15) { hw::backlight(v); delay(30); }
    for (int v = 255; v >= 0; v -= 15) { hw::backlight(v); delay(30); }
    hw::backlight(0);
    report("frontlight", "PASS", "PWM ramp on GPIO%d ran (visual check)", BOARD_BL_EN);
}

void run(bool interactive) {
    g_n = 0; g_pass = g_warn = g_fail = 0; g_hint[0] = 0;
    Serial.printf("[syscheck] start %s v%s (%s)\n", LAUNCHER_NAME, LAUNCHER_VERSION, interactive ? "interactive" : "quick");
    display::begin();
    redraw();
    checkChip();
    checkPsram();
    checkPartitions();
    checkI2C();
    checkExpander();
    checkRtc();
    checkGauge();
    checkCharger();
    checkTouch(interactive);
    checkSd();
    checkGps();
    checkLora();
    checkWifi();
    checkBle();
    checkButtons(interactive);
    checkBacklight();
    Serial.printf("[syscheck] done: %d pass, %d warn, %d fail\n", g_pass, g_warn, g_fail);
    if (interactive) {
        char s[64];
        snprintf(s, sizeof s, "%d pass, %d warn, %d fail - tap or press to leave", g_pass, g_warn, g_fail);
        hint(s);
        const uint32_t t0 = millis();
        int x, y;
        while (millis() - t0 < 60000) {
            if (hw::touchRead(x, y) || hw::bootButtonPressed() || hw::sideButtonPressed()) break;
            delay(20);
        }
    }
}

}  // namespace syscheck
