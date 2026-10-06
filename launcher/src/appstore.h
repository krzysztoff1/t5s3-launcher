#pragma once
// The SD app store: app images kept on the SD card and written into an OTA slot
// when the user picks one (docs/boot-flow.md, "Apps on the SD card").
//
//   /apps/<id>/firmware.bin   the image, exactly what `pio run` builds
//   /apps/<id>/app.ini        the manifest, `key = value` lines, # comments:
//       name    = Paperback   required: the label on HOME and in the registry
//       version = 0.3.0       optional
//       slot    = ota_1       optional: where it goes when it is not installed
//                             yet (default ota_1, the swap slot)
//       sha256  = <64 hex>    optional: SHA-256 of firmware.bin, checked while
//                             the image is copied
//
// An app is "installed" when some slot holds the very same image: the SHA-256
// that every ESP image carries in its last 32 bytes equals that slot's. That is
// a 32-byte read on the card and a cached hash of the slot, so re-opening an
// installed app costs nothing, and it stays right when OpenTrailPaper's own
// updater moves into the other slot behind the launcher's back.
#include <Arduino.h>

namespace appstore {

constexpr int MAX_APPS = 24;

struct App {
    char     id[24]       = {0};   // directory name under /apps
    char     name[32]     = {0};
    char     version[24]  = {0};
    int      slot         = 1;     // preferred OTA index
    uint32_t size         = 0;     // firmware.bin bytes
    bool     hasSha       = false;
    uint8_t  sha[32]      = {0};   // manifest sha256 of the file
    bool     hasDigest    = false;
    uint8_t  digest[32]   = {0};   // the image's own appended SHA-256
    int      installedIn  = -1;    // OTA index holding this image, -1 none
    char     problem[48]  = {0};   // non-empty: cannot be installed, says why
};

// What installing `app` would do. needsConfirm is set when the target is ota_0
// (OpenTrailPaper's home) or holds an app the SD card cannot bring back.
struct Plan {
    int      slot = -1;
    bool     already = false;      // the image is in `slot` already: just boot
    bool     needsConfirm = false;
    bool     lossy = false;        // the slot's current app is not on the card
    char     replaces[48] = {0};   // "Paperback 0.3.0", empty if the slot is empty
    uint32_t estimateMs = 0;
};

enum Phase : uint8_t { READ, WRITE, VERIFY, REGISTER, START, PHASES };

// Install progress. `done`/`total` are bytes during WRITE and 0 otherwise.
// Returning false is not supported: an install is not cancelled half-way.
using Progress = void (*)(Phase phase, uint32_t done, uint32_t total, void* ctx);

bool cardReady();                          // mounts if needed
// Every app under /apps, sorted by name; installedIn filled in. Returns count.
int  scan(App* out, int max);
bool find(const char* id, App& out);       // one app by directory name
Plan plan(const App& app, int slotOverride = -2);
// Copy the image into `slot`, verify it, register the name. Does not boot.
// On failure the slot may be half-written: its registry name is cleared and
// `err` says what happened.
bool install(const App& app, int slot, Progress cb, void* ctx, char* err, size_t errLen);

// How long installing `bytes` takes, from the last measured install rate.
uint32_t estimateMs(uint32_t bytes);
const char* phaseName(Phase p);
// "1.9 MB" / "640 KB"
void formatSize(uint32_t bytes, char* out, size_t cap);

}  // namespace appstore
