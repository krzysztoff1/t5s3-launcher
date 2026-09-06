---
name: new-app
description: Create a new app for the LilyGO T5S3 e-paper launcher from the template, build it, and install it on the connected device. Use when the user asks to "create/make/write a new app", "add an app that does X", or "build me a <thing> for the e-paper".
---

# Create a new app

Follow CLAUDE.md, section "Task: create a new app". Checklist:

1. `cp -r apps/_template apps/<kebab-name>`; edit `platformio.ini`
   (`APP_NAME`, `APP_VERSION`, `board_upload.offset_address` for the slot).
2. Implement `src/main.cpp`. Invariants: `launcher::handoff()` stays the first
   line of `setup()`; keep the `launcher` console command and the BOOT long-press
   that call `launcher::returnToLauncher()`; keep the three EPD_Painter init
   lines from the template. Pins/addresses: docs/hardware.md and
   `launcher/src/board.h`. Drivers available: SensorLib (GT911, PCF8563, XL9555),
   XPowersLib (BQ25896), BQ27220, RadioLib (SX1262), TinyGPSPlus, Adafruit GFX.
3. Add the two `check` lines for the new app to `tools/check-sync.sh`, and the
   app to `.github/workflows/build.yml`.
4. `cd apps/<name> && pio run` until it compiles cleanly.
5. Install with the install-app flow (`tools/flash.py app ota_1 ... --boot`) if a
   board is connected; otherwise say the build succeeded and how to install.

Be explicit about what is untested on hardware.
