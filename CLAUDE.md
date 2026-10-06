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
| `launcher/` | PlatformIO project: menu, crash accounting, serial console, built-in system check. |
| `apps/opentrailpaper/` | Submodule: fork of RaemondBW/OpenTrailPaper, branch `launcher`, env `t5s3-launcher`. |
| `apps/_template/` | Copy this to start a new app. |
| `launcher/src/appstore.*` | SD app store: `/apps/<id>/firmware.bin` + `app.ini`, written into a slot on demand (docs/boot-flow.md, "Apps on the SD card"). |
| `apps/paperback/` | E-book reader app (embedded sample books + SD `/books/*.txt`, Hacker News over Wi-Fi). Env `app`, slot `ota_1`. Host test: `apps/paperback/test/host/run.sh assets/books/*.txt`. |
| `tools/flash.py` | Everything USB: full install, install an app, console commands, system check. `uv` script, self-contained. |
| `tools/check-sync.sh` | Fails if any app's copy of the two shared files drifted. |
| `tools/bootstrap.sh` | Fresh clone → submodules, vendor symlink, PlatformIO packages. |
| `vendor/T5S3-4.7-e-paper-PRO/` | Submodule: LilyGO board JSON + driver libraries. |

## Commands you will run

```sh
export PATH="$HOME/.local/bin:$PATH"          # pio is installed with uv tool
cd launcher && pio run                          # build launcher -> launcher/.pio/build/launcher/firmware.bin
cd apps/opentrailpaper && pio run -e t5s3-launcher   # build OpenTrailPaper for ota_0
cd apps/paperback && pio run                    # build the Paperback reader for ota_1
tools/flash.py ports                            # is a board connected, and in which state
tools/flash.py system                           # first install: bootloader + table + launcher
tools/flash.py app ota_0 apps/opentrailpaper/.pio/build/t5s3-launcher/firmware.bin --name OpenTrailPaper --version v1.19 --boot
tools/flash.py boot ota_0                       # start an app AND stream its boot log for 30 s
tools/flash.py follow                           # after a manual RESET: stream whatever comes up
tools/flash.py cmd "list"                       # talk to the launcher (or the running app)
tools/flash.py syscheck                         # run the hardware check, prints "[syscheck] done"
tools/flash.py screenshot docs/img/x.png        # real PNG of what is on the panel right now
tools/flash.py sdapp <bin> --name X --version Y [--install]   # app into the SD store (/apps/<id>/)
tools/flash.py install <id> [--slot ota_N] [--confirm]        # SD app -> slot -> boot, follows its log
tools/flash.py put <file> /path/on/sd           # any file onto the SD card (launcher or OpenTrailPaper)
tools/flash.py monitor                          # tail serial
tools/check-sync.sh
```

The launcher's console: `help list boot apps install put ls rm register unregister
menu [home|settings|sd] syscheck touchflip nvs forget light screenshot sleep
bootloader reboot ver`. (`ls` lists SD files now; `list` is the slots.) There is no autostart:
the launcher always shows its menu on boot; an app starts only when the user
taps it or a host runs `boot`. On screen the menu is two screens: a HOME list of
the app slots (tap to boot), followed by the SD apps that are in no slot (tap → install
sheet → progress → boot; more than fit → an "SD CARD" screen), and a SETTINGS screen (touch flip, front light,
system check, sleep, flash mode behind a confirm sheet); BOOT short = next, held
= select; side short = next, held = back; the GT911 capacitive key goes back.
OpenTrailPaper's console understands `launcher` (hand back) and `bootloader`.

## Task: install an app on the connected device

Two ways. **Resident**: write a slot over USB (below). **SD app** (the right
choice for anything beyond OpenTrailPaper + one app):
`tools/flash.py sdapp <bin> --name "<Name>" --version <ver> --install`. It
copies the image to `/apps/<id>/` on the card (about 1 MB per 10 s over USB),
then installs it into its slot (default `ota_1`) and boots it. If the target is
`ota_0` or holds an app that is not on the card, the launcher refuses until you
pass `--confirm`. Ask the user before you do. Re-running `install <id>` when the
image is already in a slot just boots it.

1. `tools/flash.py ports`. No device → tell the user to plug it in (data cable).
2. Have a `firmware.bin` built for a 16 MB ESP32-S3 (build it if it is a repo).
3. Pick the slot: `ota_0` is OpenTrailPaper's home, `ota_1` its update target;
   a second app goes in `ota_1` and the user must know OpenTrailPaper's next
   self-update can replace it (docs/boot-flow.md, "two OTA slots").
4. `tools/flash.py app <slot> <bin> --name "<Name>" --version <ver> --boot`.
5. Confirm: the command prints `[launcher] registered` and `[launcher] booting`,
   then streams the app's boot log for 30 s (`--follow SEC` to change). Read it
   for errors. OpenTrailPaper's first seconds are only in its SD log
   (`/logs/YYYYMMDD.log`) because its USB starts after the SD mount; a healthy
   boot shows `[rec] SD ready`, `[main] all tasks started`, `usb storage: MSC ready`.
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

* SD app store (2026-10-06, v0.3.0): built, NOT yet run on a board (none was
  connected). Unverified: `put` throughput, the install itself, the slot-map
  progress screen, the HOME SD rows, `inst_kbps` estimates.
* Never change `partitions.csv` casually: it forces a full reflash of every
  slot. Never edit the copies in apps; change the root file and re-copy.
* Exactly two OTA slots (see boot-flow.md). Do not add `ota_2`.
* Platform is pinned: `espressif32@6.5.0` (Arduino 2.0.14 / ESP-IDF 4.4) for
  every project so images agree on the bootloader. Use IDF 4.4 API names.
* Arduino images cannot carry a project name; the label comes from
  `launcher::handoff()` or `flash.py app --name`. Do not try to patch
  `esp_app_desc`.
* Hardware status: launcher install, OpenTrailPaper + Paperback install and
  hand-back, and the full system check (16 pass on 2026-09-07) verified on a
  board. Touch worked without `touchflip`. Still unverified: side-button-through-
  RESET, deep-sleep wake into the menu.
* UI redesign (2026-09-07, v0.2.0): the menu is now HOME (app-slot list) + a
  separate SETTINGS screen, every touch target >= 88 px, OpenTrailPaper's status
  bar / list-row / switch / stepper / confirm-sheet vocabulary (`ui.cpp`,
  helpers in `display.cpp`). syscheck redraws in the same status bar. The
  capacitive GT911 key (`hw::homeKeyPressed()`) is wired as "back". Built and
  system-check-clean on the board; the touch layout itself is unverified by a
  human tapping it.
* No autostart (2026-09-07): the launcher always shows its menu on boot; an app
  starts only on a tap or `flash.py boot`. `main.cpp` has no countdown, Settings
  has no autostart switch, and the `autostart` console command is gone. The
  `autostart` NVS key is now ignored (registry still defines the accessors).
* Paperback (2026-09-06): verified on the board: install, library with cover
  art, page layout and turns (10-25 ms per page), console, Hacker News over
  Wi-Fi (join, NTP clock, front page, article via r.jina.ai). Still unverified:
  sleep/wake footer repair, SD-card books and covers, the shared front light
  round trip between apps.
* The front light is one device-wide setting: `launcher::frontLight()` /
  `setFrontLight(duty)` in `launcher_api.h` (NVS key `light`, PWM duty 0..255,
  steps 0/12/90/230 = off/low/medium/high). The launcher applies it at boot and
  has a menu item; Paperback and OpenTrailPaper read it at boot and write it
  whenever the user changes the light. A new app must do the same.
* Screenshots are real captures, never mock-ups: `launcher::dumpScreen()` in
  `launcher_api.h` run-length encodes the 4-level framebuffer over the console
  and `tools/flash.py screenshot <file>.png` decodes it. The launcher, Paperback
  and the template have the `screenshot` command; OpenTrailPaper does not (its
  framebuffer is epdiy's 4bpp landscape one). `docs/img/*.png` are the README's,
  re-take them when a screen changes.
* Flash/serial output is the ground truth. When something "should work", run
  `tools/flash.py monitor` and read what the device says.
