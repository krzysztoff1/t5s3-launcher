#pragma once
// The launcher's view of the flash: which app partitions exist, which hold a
// valid image, what they are called — plus the small NVS state shared with the
// apps through launcher_api.h (same namespace, same keys).
#include <Arduino.h>
#include <esp_partition.h>
#include <esp_system.h>

namespace registry {

struct Slot {
    const esp_partition_t* part = nullptr;
    int  index = -1;          // OTA index; -1 = factory (the launcher)
    bool valid = false;       // holds a bootable app image
    bool running = false;
    bool last = false;        // the app that ran most recently
    char name[32] = {0};
    char version[32] = {0};
};

// Factory first, then ota_0..ota_n in order. Returns the count written.
int scan(Slot* out, int max);
const esp_partition_t* otaPartition(int index);
const char* slotLabel(int index);         // "factory" / "ota_0" ...
int slotFromLabel(const char* label);     // "ota_1" or "1" -> 1; -1 factory; -2 unknown

// Point otadata at an OTA slot and restart into it. Records the reset reason
// that led here (for the app's launcher::previousResetReason()) and arms the
// crash counter. Returns only on failure.
bool boot(int index, esp_reset_reason_t reasonToStore);

int  lastSlot();                          // -1 if none
void setLastSlot(int index);
bool autostart();
void setAutostart(bool on);
bool consumeShowMenu();
bool consumeArmed();
uint8_t crashes();
void setCrashes(uint8_t n);
bool touchFlip();
void setTouchFlip(bool on);
void setName(int index, const char* name, const char* version);
void clearName(int index);
void forgetAll();
void dump(Print& out);

}  // namespace registry
