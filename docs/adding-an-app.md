# Adding an app

Two ways in: install an existing firmware image, or create a new app from the
template. Either way the device ends up with the image in an OTA slot and a
label in the launcher menu.

## Install an existing image

```sh
tools/flash.py app ota_1 path/to/firmware.bin --name "Name" --version v1.0 --boot
```

`app` writes the image to the slot, blanks otadata, resets, waits for the
launcher's console, registers the label and (with `--boot`) starts it. The image
must be an ESP32-S3 Arduino/IDF app built for a 16 MB flash; it does **not** need
the launcher hook — an app without it simply stays the boot target until you
hold the side button through a RESET (or reflash). Apps *with* the hook get the
crash accounting and the automatic return to the launcher on cold boot.

Slot rule: `ota_0` and `ota_1` are OpenTrailPaper's pair (it self-updates
between them). Installing something else into `ota_1` is fine as long as you
accept OpenTrailPaper's next update may replace it.

## Create a new app

```sh
cp -r apps/_template apps/my-app
cd apps/my-app
# edit platformio.ini: APP_NAME, APP_VERSION, board_upload.offset_address (which slot)
# edit src/main.cpp
pio run
../../tools/flash.py app ota_1 .pio/build/app/firmware.bin --name "My App" --version v0.1 --boot
```

Rules for an app that plays well:

1. `launcher::handoff(APP_NAME, APP_VERSION)` is the first line of `setup()`.
2. Offer a way back: a console command `launcher` and/or a long press calling
   `launcher::returnToLauncher()`. The template does both.
3. Keep `partitions_launcher.csv` and `src/launcher_api.h` as verbatim copies of
   the repo-root files. `tools/check-sync.sh` fails CI if they drift.
4. Use the same `platform = espressif32@6.5.0` as the launcher so the images
   agree on the bootloader and IDF.
5. If the app uses the e-paper through EPD_Painter, copy the three init lines
   from the template (`cfg.i2c.wire = &Wire`, `setAutoShutdown(false)`,
   portrait canvas). See docs/hardware.md.

## Port an existing project

Same as above but inside the project's own PlatformIO env: add
`board_build.partitions = partitions_launcher.csv`,
`board_upload.offset_address = 0x310000` (ota_0) or `0x910000` (ota_1), copy the
two files in, add the hook. `apps/opentrailpaper` (branch `launcher`) is the
worked example: `git diff main..launcher` there is the whole port.
