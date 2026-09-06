# t5s3-launcher — agent runbook

Multi-app launcher for the LilyGO T5S3 4.7" e-paper PRO (ESP32-S3, 16 MB flash,
8 MB PSRAM). The launcher lives in the `factory` partition; apps live in
`ota_0`/`ota_1`. Read `docs/boot-flow.md` once; the rest of this file is what to
*do*.

## Map

| Path | What |
|---|---|
| `partitions.csv` | THE flash layout. Apps carry verbatim copies; never edit a copy. |
| `launcher_api/launcher_api.h` | Header apps include. `handoff()` first line of `setup()`, `returnToLauncher()` to go back. Apps carry verbatim copies. |
| `launcher/` | PlatformIO project: menu, autostart, crash accounting, serial console, built-in system check. |
| `apps/opentrailpaper/` | Submodule: fork of RaemondBW/OpenTrailPaper, branch `launcher`, env `t5s3-launcher`. |
| `apps/_template/` | Copy this to start a new app. |
| `tools/flash.py` | Everything USB: full install, install an app, console commands, system check. `uv` script, self-contained. |
| `tools/check-sync.sh` | Fails if any app's copy of the two shared files drifted. |
| `tools/bootstrap.sh` | Fresh clone → submodules, vendor symlink, PlatformIO packages. |
| `vendor/T5S3-4.7-e-paper-PRO/` | Submodule: LilyGO board JSON + driver libraries. |

## Commands you will run

```sh
export PATH="$HOME/.local/bin:$PATH"          # pio is installed with uv tool
cd launcher && pio run                          # build launcher -> launcher/.pio/build/launcher/firmware.bin
cd apps/opentrailpaper && pio run -e t5s3-launcher   # build OpenTrailPaper for ota_0
tools/flash.py ports                            # is a board connected, and in which state
tools/flash.py system                           # first install: bootloader + table + launcher
tools/flash.py app ota_0 apps/opentrailpaper/.pio/build/t5s3-launcher/firmware.bin --name OpenTrailPaper --version v1.19 --boot
tools/flash.py cmd "list"                       # talk to the launcher (or the running app)
tools/flash.py syscheck                         # run the hardware check, prints "[syscheck] done"
tools/flash.py monitor                          # tail serial
tools/check-sync.sh
```

The launcher's console: `help list boot register unregister autostart menu
syscheck touchflip nvs forget sleep bootloader reboot ver`. OpenTrailPaper's
console understands `launcher` (hand back) and `bootloader`.

## Task: install an app on the connected device

1. `tools/flash.py ports`. No device → tell the user to plug it in (data cable).
2. Have a `firmware.bin` built for a 16 MB ESP32-S3 (build it if it is a repo).
3. Pick the slot: `ota_0` is OpenTrailPaper's home, `ota_1` its update target;
   a second app goes in `ota_1` and the user must know OpenTrailPaper's next
   self-update can replace it (docs/boot-flow.md, "two OTA slots").
4. `tools/flash.py app <slot> <bin> --name "<Name>" --version <ver> --boot`.
5. Confirm: the command prints `[launcher] registered` and `[launcher] booting`.
   Then `tools/flash.py monitor` for ~10 s to see the app come up.
6. If the launcher itself is not on the device yet (`system` never run, or the
   console never answers `list`), run `tools/flash.py system` first.

## Task: create a new app and put it on the device

1. `cp -r apps/_template apps/<name>`; set `APP_NAME`, `APP_VERSION` and
   `board_upload.offset_address` (0x310000 = ota_0, 0x910000 = ota_1) in its
   `platformio.ini`; write `src/main.cpp`. Keep `launcher::handoff()` as the
   first line of `setup()` and keep a way back (`launcher` console command and
   a long-press are in the template).
2. Add the two check lines for it to `tools/check-sync.sh`.
3. `cd apps/<name> && pio run` — fix until it builds.
4. Install as above. Add the app to the CI matrix in `.github/workflows/build.yml`.
5. Do not copy EPD_Painter init from anywhere but the template or
   `launcher/src/display.cpp`; the three lines there are hard-won
   (`cfg.i2c.wire = &Wire`, `setAutoShutdown(false)`, PSRAM canvas).

## Task: change the launcher

Edit `launcher/src/*`, `cd launcher && pio run`, then `tools/flash.py launcher`
(rewrites only the factory slot and blanks otadata). Bump `LAUNCHER_VERSION` in
`launcher/src/version.h` for anything user-visible.

## Task: update OpenTrailPaper from upstream

```sh
cd apps/opentrailpaper && git fetch origin && git fetch https://github.com/RaemondBW/OpenTrailPaper main:upstream-main
git rebase upstream-main          # on branch `launcher`; the port is ~40 lines in 3 files + 2 copied files
pio run -e t5s3-launcher && git push -f origin launcher
cd ../.. && git add apps/opentrailpaper && git commit -m "Track OpenTrailPaper <version>"
```
Keep `FIRMWARE_VERSION` from upstream's `src/config.h` as the `--version` you register.

## Rules

* Never change `partitions.csv` casually: it forces a full reflash of every
  slot. Never edit the copies in apps; change the root file and re-copy.
* Exactly two OTA slots (see boot-flow.md). Do not add `ota_2`.
* Platform is pinned: `espressif32@6.5.0` (Arduino 2.0.14 / ESP-IDF 4.4) for
  every project so images agree on the bootloader. Use IDF 4.4 API names.
* Arduino images cannot carry a project name; the label comes from
  `launcher::handoff()` or `flash.py app --name`. Do not try to patch
  `esp_app_desc`.
* Nothing in this repo has been validated on hardware yet except by
  compilation (2026-09-06). First things to check on a real board: the panel
  comes up (display init log line), touch orientation (`touchflip`), and the
  side-button-through-RESET path. Say so plainly when reporting.
* Flash/serial output is the ground truth. When something "should work", run
  `tools/flash.py monitor` and read what the device says.
