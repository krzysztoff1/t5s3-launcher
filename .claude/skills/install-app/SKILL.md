---
name: install-app
description: Install a firmware image into an OTA slot on the connected LilyGO T5S3 e-paper board and register it in the launcher menu. Use when the user says "install X on the device", "flash this app", "put OpenTrailPaper on the board", or hands over a firmware.bin / a project to build and install.
---

# Install an app on the connected device

Follow the runbook in CLAUDE.md, section "Task: install an app". In short:

1. `tools/flash.py ports` — stop and ask for the cable if nothing shows up.
2. Get a `firmware.bin`: build the project if given one (`pio run`, note the env).
   For OpenTrailPaper: `cd apps/opentrailpaper && pio run -e t5s3-launcher`.
3. Choose the slot with the user's intent in mind: `ota_0` = OpenTrailPaper,
   `ota_1` = second app (warn once that OpenTrailPaper's self-update can evict it).
4. `tools/flash.py app <slot> <bin> --name "<Name>" --version <ver> --boot`
5. Verify from the output: `[launcher] registered`, `[launcher] booting`, then the
   app's boot log streams for 30 s. Report errors or watchdog/panic lines; for
   OpenTrailPaper the healthy markers are `[rec] SD ready`, `[main] all tasks
   started`, `usb storage: MSC ready`.
6. If the launcher is missing on the device, `tools/flash.py system` first, then retry.

Report what was flashed where, and anything the device printed that looks wrong.
