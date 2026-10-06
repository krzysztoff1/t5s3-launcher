#include "sdcard.h"

#include <SD.h>
#include <SPI.h>
#include <string.h>

#include "board.h"

namespace sdcard {

static bool g_spi = false;
static bool g_mounted = false;
static uint32_t g_failedAt = 0;
constexpr uint32_t RETRY_MS = 5000;

void spiBegin() {
    if (g_spi) return;
    pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);   digitalWrite(BOARD_SD_CS, HIGH);
    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI, BOARD_SD_CS);
    g_spi = true;
}

bool mount(bool force) {
    if (g_mounted) return true;
    if (!force && g_failedAt && millis() - g_failedAt < RETRY_MS) return false;
    spiBegin();
    g_mounted = SD.begin(BOARD_SD_CS, SPI, 20000000);
    g_failedAt = g_mounted ? 0 : (millis() | 1);
    return g_mounted;
}

bool mounted() { return g_mounted; }

void unmount() {
    if (g_mounted) SD.end();
    g_mounted = false;
}

bool remount() {
    unmount();
    return mount(true);
}

void mkdirsFor(const char* path) {
    char dir[96];
    for (const char* p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        const size_t n = p - path;
        if (n >= sizeof dir) return;
        memcpy(dir, path, n);
        dir[n] = 0;
        if (!SD.exists(dir)) SD.mkdir(dir);
    }
}

}  // namespace sdcard
