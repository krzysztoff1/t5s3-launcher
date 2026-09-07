#pragma once
// Reader preferences, persisted as one NVS blob ("paperback"/"cfg").
#include <stdint.h>

struct Settings {
    uint8_t version;      // bump when the struct changes
    uint8_t fontFamily;   // 0 serif (Literata), 1 sans (Inter)
    uint8_t sizeIdx;      // 0..6
    uint8_t spacing;      // 0 compact, 1 normal, 2 relaxed
    uint8_t margin;       // 0 narrow, 1 normal, 2 wide
    uint8_t justify;      // 0 left, 1 justified
    uint8_t light;        // front light 0..3 (off, low, medium, high); mirrors the shared duty
    uint8_t fastTurn;     // 0 quality, 1 fast page turns
    uint8_t refreshIdx;   // full refresh: 0 off, 1 every 5, 2 every 10, 3 every 20 pages
    uint8_t sleepIdx;     // idle sleep: 0 5 min, 1 10 min, 2 30 min, 3 never
    uint8_t tzIdx;        // time zone (see settings::tzName)
    uint8_t hints;        // bit0: reader gesture hint shown
    float   secPerPage;   // reading pace (EMA), 0 = unknown
    char    wifiSsid[33];
    char    wifiPass[65];
};

namespace settings {

constexpr int SIZE_COUNT = 7;
constexpr int TZ_COUNT = 12;

Settings& get();
void load();
void save();          // writes only if changed since load/save

float    bodyPx();
int      lineHeight();
int      marginPx();
uint32_t sleepMs();   // 0 = never
int      refreshEvery();   // pages, 0 = off
const char* tzName(int idx);
const char* tzPosix(int idx);
uint8_t  lightDuty();               // PWM duty for the current level
uint8_t  lightLevelFromDuty(uint8_t duty);
const char* lightName(int level);

}  // namespace settings
