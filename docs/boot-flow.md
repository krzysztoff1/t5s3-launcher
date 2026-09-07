# Boot flow

The device has one bootloader, one partition table and up to three app images.
The launcher owns the first two and lives in the `factory` slot; apps live in
`ota_0` and `ota_1`.

```
power-on / RESET / crash / esp_restart()
        │
        ▼
 2nd-stage bootloader reads otadata
        │
        ├─ otadata blank ──────────────► factory = LAUNCHER
        │                                  │
        │                                  └─► MENU (always; no autostart)
        │                                        │
        │                                        └─ user taps an app (or flash.py boot):
        │                                           esp_ota_set_boot_partition(ota_N) + restart
        │                                              │
        └─ otadata → ota_N ◄───────────────────────────┘
                 │
                 ▼
              APP starts; first line of setup(): launcher::handoff()
                 │   · registers slot/name/version in NVS
                 │   · ERASES otadata  ──► next cold boot is the launcher again
                 ▼
              app runs; deep sleep / wake cycles stay inside the app
```

## Why the app erases otadata on every boot

So that the launcher is always one RESET away, with no bootloader changes:

* **The launcher is always the menu.** There is no autostart: a cold boot,
  RESET, or crash all land in the menu. Hold the side button through a RESET only
  if the running app never handed back (otadata still points at it).
* **A crashing app cannot lock you out.** A panic resets the chip, the launcher
  comes up in its menu and notes the crash reason. Because it never starts an app
  on its own, a crash-looping app simply drops you back in the menu.
* **Deep sleep is unaffected.** The stock Arduino bootloader is built with
  `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`, so a wake from deep sleep
  fast-boots the partition recorded in RTC memory *before* otadata is even read
  (ESP-IDF 4.4 `bootloader_start.c`). OpenTrailPaper's sleep/wake cycle never
  sees the launcher.

The cost is a launcher hop on every cold boot: it draws the menu and waits.
There is no countdown and no autostart — an app starts only when you tap it or a
host runs `tools/flash.py boot`. The launcher's console is live the whole time,
so `register`/`boot` from `tools/flash.py` need no timing window.

## Reset reason plumbing

Because the launcher reaches an app through `esp_restart()`, the app's own
`esp_reset_reason()` reads `ESP_RST_SW` even after a crash. The launcher stores
the true reason in NVS (`app_rr`) right before starting the app, and
`launcher::previousResetReason()` returns it. OpenTrailPaper's fork uses that
for its "save the core dump after a crash" path.

## Facts verified against the sources

| Claim | Where |
|---|---|
| `esp_ota_set_boot_partition(factory)` erases otadata | esp-idf v4.4.6 `esp_ota_ops.c` |
| Deep-sleep wake boots from RTC memory before otadata | esp-idf v4.4.6 `bootloader_start.c`, `bootloader_utility.c` |
| Arduino auto-confirms OTA images at boot (`verifyRollbackLater`) | arduino-esp32 2.0.14 `esp32-hal-misc.c` |
| Rollback + skip-validate enabled in the stock bootloader | arduino-esp32 2.0.14 `tools/sdk/esp32s3/sdkconfig` |
| PlatformIO uploads at `board_upload.offset_address` (default 0x10000) | platform-espressif32 6.5.0 `builder/main.py` |

## NVS state (namespace `launcher`)

| key | type | meaning |
|---|---|---|
| `last_slot` | u8 | OTA index of the app that last ran |
| `autostart` | u8 | ignored since v0.2.0 (the launcher always shows the menu) |
| `show_menu` | u8 | set by `launcher::returnToLauncher()`, consumed by the launcher |
| `armed` | u8 | set right before starting an app; a crash reset while armed counts |
| `crashes` | u8 | consecutive crashes; counted and logged, no longer pauses anything |
| `app_rr` | u8 | true reset reason handed to the app |
| `tflip` | u8 | rotate touch 180° (see hardware.md); live on SETTINGS |
| `light` | u8 | front-light PWM duty, shared with every app |
| `n<i>`, `v<i>` | str | display name / version registered for `ota_<i>` |

## Why exactly two OTA slots

OpenTrailPaper updates itself (BLE from the phone, or `firmware.bin` on the SD
card) through Arduino's `Update` library, which always writes to *the next OTA
slot in rotation*. With `ota_0`/`ota_1` it ping-pongs between its own two slots
and never touches the launcher. A third OTA slot would be overwritten by its
next update. If a third resident app is ever needed, either patch
OpenTrailPaper's updater to target its sibling slot, or install apps from the SD
card on demand (not built yet).
