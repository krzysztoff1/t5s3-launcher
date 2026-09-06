---
name: device-check
description: Run the launcher's built-in hardware system check on the connected LilyGO T5S3 e-paper board and summarise the results. Use when the user asks "is the device ok", "check the hardware", "run the system check", or reports that some peripheral (GPS, SD, touch, LoRa, battery) seems broken.
---

# Device check

1. `tools/flash.py ports`. If OpenTrailPaper (OTG port) is running, first
   `tools/flash.py cmd "launcher" --wait 8` to hand back to the launcher.
2. `tools/flash.py syscheck` (waits for `[syscheck] done`). The interactive
   steps (touch, buttons) wait a few seconds each for a human; if nobody is at
   the device use `tools/flash.py cmd "syscheck quick" --wait 90`.
3. Summarise per line: name, PASS/WARN/FAIL, the detail. Explain WARN/FAIL in
   plain words (e.g. "no SD card inserted", "GPS silent at both baud rates — the
   3V3 rail is switched by the expander; check the expander line first").
4. Anything below PASS on `display`-related items or `slots` is worth flagging
   before the user flashes anything else.
