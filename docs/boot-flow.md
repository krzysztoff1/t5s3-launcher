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
| `inst_kbps` | u16 | measured SD-install throughput, for the "about N s" estimates |
| `n<i>`, `v<i>` | str | display name / version registered for `ota_<i>` |

## Why exactly two OTA slots

OpenTrailPaper updates itself (BLE from the phone, or `firmware.bin` on the SD
card) through Arduino's `Update` library, which always writes to *the next OTA
slot in rotation*. With `ota_0`/`ota_1` it ping-pongs between its own two slots
and never touches the launcher. A third OTA slot would be overwritten by its
next update. More apps than slots are handled by keeping them on the SD card
and writing one into a slot when it is picked (next section). The alternative,
patching OpenTrailPaper's updater to target its sibling slot, is not needed.

## Apps on the SD card

The launcher keeps any number of app images on the SD card and copies one into
an OTA slot when you pick it.

```
/apps/<id>/firmware.bin    the image, exactly what `pio run` builds
/apps/<id>/app.ini         the manifest:
    name    = Paperback    # required: the label on HOME and in the registry
    version = 0.3.0        # optional
    slot    = ota_1        # optional: where it goes when not installed (default ota_1)
    sha256  = 9f2c...      # optional: SHA-256 of firmware.bin, checked while copying
```

`tools/flash.py sdapp fw.bin --name X --version Y [--install]` writes both files
over USB (console `put`). Copying them with a card reader works just as well.

**Installed or not** is decided by the image itself. Every ESP image ends in a
SHA-256 of itself (the same digest `esp_partition_get_sha256()` returns for a
slot). If the last 32 bytes of `firmware.bin` equal a slot's digest, that slot
already holds the app, and picking it boots that slot with no copy. That costs
one 32-byte read per app and one hash per slot per boot, and it stays correct
whatever moved images around behind the launcher's back.

**Installing** (`appstore.cpp`) runs `esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES)`,
then `esp_ota_write` in 8 KB chunks read from the card. Erasing sector by sector
gets the progress moving at once. The task yields every 64 KB so the idle task
and its watchdog run. The file's SHA-256 is checked against `app.ini` if it has
one, and `esp_ota_end` validates the image (segments, checksum, appended digest).
The slot is read back and compared with the image's digest, the name and version
are written to `n<i>`/`v<i>` (the same keys `flash.py app --name` uses), and the
slot is booted through the normal path. SD reads and flash writes alternate in
one task, and the panel repaints between chunks, never during one. If anything
fails after `esp_ota_begin`, the slot is half-written: its label is cleared, so
HOME shows it as empty, and a retry writes it again.

**Slot policy**

1. If some slot already holds the image, that slot is booted. No write.
2. Otherwise the target is the manifest's `slot` (default `ota_1`, the swap slot).
3. A confirm sheet always shows the size, the target slot, the estimated time and
   what gets replaced. It defaults to *Cancel*, and the console wants
   `install <id> <slot> confirm`, when the target is `ota_0` (OpenTrailPaper's
   home) or holds an app whose image is **not** on the card. Replacing that app
   is one-way: only `tools/flash.py app` brings it back. Replacing an app that is
   on the card defaults to *Install*, because it can be re-installed in seconds.

**Interaction with OpenTrailPaper's updater.** Its updater writes the slot after
its own: running from `ota_0`, it overwrites `ota_1`, which is wherever the SD
app is. Afterwards OpenTrailPaper runs from `ota_1`, `ota_0` holds its old
version, and its `handoff()` relabels `ota_1`. Nothing is lost, because the SD
app is still on the card: picking it again installs it into `ota_1`, its
manifest slot. That is now OpenTrailPaper's slot and not on the card, so the
sheet asks first. Either answer is safe: install into `ota_1` and then put the
new OpenTrailPaper back with `flash.py app`, or set `slot = ota_0` in the
manifest (or `install <id> ota_0 confirm`) to overwrite the stale copy instead.
The rule of thumb: keep OpenTrailPaper in one slot, and let SD apps take turns
in the other.
