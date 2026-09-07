# Recovery

**Menu will not come up, app keeps starting.** Hold the side button, press and
release RESET, keep holding until the menu is on screen. If the app is one
without the launcher hook, otadata still points at it; the same button hold
works only if the app erased otadata, so instead reflash otadata blank:
`tools/flash.py launcher` (rewrites the launcher and blanks otadata).

**Device is silent, no USB port at all.** Hold BOOT, tap RESET, release BOOT.
The ROM loader enumerates as "USB JTAG/serial debug unit". Then
`tools/flash.py system` reinstalls bootloader + table + launcher (settings in
NVS survive; add `--erase-nvs` to wipe them).

**Stuck in download mode after a flash.** OpenTrailPaper's `bootloader` command
sets a sticky RTC bit that only a physical RESET or `tools/flash.py`'s finish
step clears. Press RESET once, or run `tools/flash.py launcher`.

**Launcher says an app crashed.** The menu is up (it always is — there is no
autostart). The note shows the crash reason; start the app again by tapping it,
read the app's own logs, or `tools/flash.py monitor` right after tapping to catch
the crash.

**Changed partitions.csv.** Everything must be reflashed: `tools/flash.py
system`, then `tools/flash.py app ...` for each slot. NVS keeps Arduino's stock
offset so settings survive unless you also moved `nvs`.

**Want the stock single-app layout back.** Flash OpenTrailPaper's own release
with its web flasher or `pio run -e t5s3-painter -t upload` from a checkout on
`main`; that writes Arduino's default partition table over ours.
