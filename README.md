# t5s3-launcher

A launcher for running several firmwares on one **LilyGO T5S3 4.7" e-paper PRO**
(ESP32-S3, 16 MB flash, 8 MB PSRAM). The launcher sits in the flash's `factory`
partition and shows a menu on the e-paper; each app lives in its own OTA slot
and is a normal, independent firmware. Pick an app on screen, it boots; hold the
side button through a reset, you are back in the menu.

Apps today:

* **OpenTrailPaper** — the e-paper bike computer, as a fork with a ~40-line port
  (`apps/opentrailpaper`, branch `launcher`).
* **System check** — built into the launcher: every chip on the board, results
  on screen and over serial.

## How it works

The stock ESP32 bootloader picks the `factory` partition whenever the OTA data
is blank, and an OTA slot otherwise. The launcher starts an app by pointing the
OTA data at its slot and restarting. The app, as the first line of `setup()`,
calls `launcher::handoff()`, which registers itself and blanks the OTA data
again — so every cold boot passes through the launcher (which autostarts the last
app unless a button is held), while deep-sleep wakes fast-boot the app directly
and never see the launcher. Details and the verified bootloader facts:
[docs/boot-flow.md](docs/boot-flow.md).

```
0x000000 bootloader      0x010000 factory  = launcher (3 MB)
0x008000 partition table 0x310000 ota_0    = OpenTrailPaper (6 MB)
0x009000 nvs             0x910000 ota_1    = OpenTrailPaper's update target (6 MB)
0x00E000 otadata         0xF10000 storage  (896 KB)   0xFF0000 coredump (64 KB)
```

## Setup

```sh
uv tool install --python 3.13 platformio     # if you do not have pio
brew install esptool                          # optional; tools/flash.py brings its own
git clone --recursive git@github.com:krzysztoff1/t5s3-launcher.git
cd t5s3-launcher && tools/bootstrap.sh
```

## Build and install

```sh
(cd launcher && pio run)
(cd apps/opentrailpaper && pio run -e t5s3-launcher)

tools/flash.py system     # once: bootloader + partition table + launcher
tools/flash.py app ota_0 apps/opentrailpaper/.pio/build/t5s3-launcher/firmware.bin \
    --name OpenTrailPaper --version v1.19 --boot
```

`tools/flash.py` handles both of the board's USB personalities (the launcher's
USB-Serial-JTAG and OpenTrailPaper's USB-OTG), so no BOOT/RESET presses are
needed. `tools/flash.py --help` lists the rest: `ports`, `cmd`, `boot` (starts
an app and streams its boot log), `follow`, `syscheck`, `monitor`.

## Using it

* **Menu**: tap an app to start it. BOOT short-press moves the highlight, long
  press selects; the side button also moves it. Idle five minutes on battery →
  deep sleep; BOOT wakes.
* **Back to the launcher from an app**: OpenTrailPaper's console command
  `launcher`, or hold the **side button** while pressing RESET.
* **Autostart**: on by default; a cold boot goes straight to the last app after
  a short countdown. Toggle in the menu or `tools/flash.py cmd "autostart off"`.
* **Crash loop**: three crashes in a row pause autostart and the menu says so.
* **System check**: menu entry, or `tools/flash.py syscheck` from the Mac.

## Adding apps

See [docs/adding-an-app.md](docs/adding-an-app.md) — install any image with
`tools/flash.py app`, or copy `apps/_template`. Working with Claude Code? The
repo's `CLAUDE.md` is the runbook, and `/install-app`, `/new-app`,
`/device-check` are ready-made prompts.

## Status

Running on a board since 2026-09-06: install, menu, touch, autostart,
OpenTrailPaper hand-back and the system check (every peripheral PASS) verified.
Not yet exercised: the crash-loop pause, side-button-through-RESET and
deep-sleep wake into the menu. See [docs/recovery.md](docs/recovery.md) if a
flash goes wrong.

## Layout

```
partitions.csv          shared flash layout (single source of truth)
launcher_api/           header apps include
launcher/               the launcher firmware (PlatformIO)
apps/opentrailpaper/    submodule: fork, branch `launcher`
apps/_template/         starting point for a new app
tools/                  flash.py, check-sync.sh, bootstrap.sh
docs/                   boot-flow, hardware, adding-an-app, recovery
vendor/                 submodule: LilyGO board support + drivers
```

## Licenses

Launcher code: MIT. OpenTrailPaper is Apache-2.0 (its own LICENSE in the
submodule); EPD_Painter, SensorLib, XPowersLib, RadioLib and the LilyGO board
support keep their own licenses.
