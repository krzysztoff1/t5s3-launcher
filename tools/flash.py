#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "esptool>=5.0"]
# ///
"""t5s3-launcher device tool: flash the launcher, install apps, talk to the console.

Run it directly (uv resolves pyserial + esptool on first use):

    tools/flash.py ports                       # what is connected, and in which state
    tools/flash.py partitions                  # the shared flash layout
    tools/flash.py system                      # bootloader + table + launcher (full install)
    tools/flash.py launcher                    # re-flash only the launcher (factory slot)
    tools/flash.py app ota_0 fw.bin --name OpenTrailPaper --version v1.19 --boot
    tools/flash.py cmd "list"                  # send a console command, print the reply
    tools/flash.py boot ota_0                  # start a slot and follow its boot log live
    tools/flash.py follow                      # after a manual reset: stream whatever comes up
    tools/flash.py syscheck                    # run the built-in system check, print results
    tools/flash.py monitor                     # plain serial monitor (Ctrl-C to stop)

Design notes
------------
* The launcher owns the bootloader and the partition table (`system`). Apps only
  ever write their own slot (`app`), plus a blank otadata so the device comes up
  in the launcher afterwards — which is where the registration/boot commands go.
* Two USB personalities exist on this board. The launcher (and the ROM loader)
  use the USB-Serial-JTAG peripheral, whose DTR/RTS lines reach EN/GPIO0, so
  esptool's normal auto-reset works. OpenTrailPaper runs USB in OTG mode for
  mass storage, where auto-reset is impossible; it is asked to reboot into the
  ROM loader with its `bootloader` console command instead. Both paths end in
  the same place, so everything below is written against "a board in download
  mode on port X" and only the entry differs.
* Leaving download mode needs two things esptool gets wrong on this bridge (see
  OpenTrailPaper's tools/flash.py for the long version): clear the sticky
  RTC_CNTL_FORCE_DOWNLOAD_BOOT bit, then reset with DTR (GPIO0) explicitly high.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time

import serial
import serial.tools.list_ports as list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARTITIONS_CSV = os.path.join(ROOT, "partitions.csv")
LAUNCHER_BUILD = os.path.join(ROOT, "launcher", ".pio", "build", "launcher")

ESPRESSIF_VID = 0x303A
BOOTLOADER_OFFSET = 0x0
PARTITION_TABLE_OFFSET = 0x8000

# soc/rtc_cntl_reg.h: DR_REG_RTCCNTL_BASE (0x60008000) + 0x12C, bit 0.
RTC_CNTL_OPTION1_REG = 0x6000812C
RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1

DOWNLOAD_PORT_TIMEOUT = 20.0
CONSOLE_PORT_TIMEOUT = 30.0
CONSOLE_BAUD = 115200


def log(msg: str) -> None:
    print(f"[flash] {msg}", flush=True)


# --- partition table --------------------------------------------------------
def parse_partitions(path: str = PARTITIONS_CSV) -> list[dict]:
    parts = []
    with open(path) as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) < 5:
                continue
            parts.append({
                "name": cols[0], "type": cols[1], "subtype": cols[2],
                "offset": int(cols[3], 0), "size": int(cols[4], 0),
            })
    return parts


def app_slot(name_or_index: str) -> dict:
    """Resolve 'ota_0' / '0' / 'factory' to a partition entry."""
    key = name_or_index.lower()
    if key.isdigit():
        key = f"ota_{key}"
    for p in parse_partitions():
        if p["type"] == "app" and p["name"].lower() == key:
            return p
    slots = ", ".join(p["name"] for p in parse_partitions() if p["type"] == "app")
    raise SystemExit(f"[flash] unknown app slot {name_or_index!r}; app slots are: {slots}")


def otadata_partition() -> dict:
    for p in parse_partitions():
        if p["type"] == "data" and p["subtype"] == "ota":
            return p
    raise SystemExit("[flash] partitions.csv has no otadata partition")


# --- port discovery ---------------------------------------------------------
def esp_ports():
    return [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]


def is_jtag_port(p) -> bool:
    # The USB-Serial-JTAG peripheral reports this product string whether the ROM
    # loader or an app (the launcher) is behind it; an OTG-mode app reports the
    # board name from its own descriptors.
    return "jtag" in (p.product or "").lower()


def find_jtag_port():
    for p in esp_ports():
        if is_jtag_port(p):
            return p.device
    return None


def find_otg_port():
    for p in esp_ports():
        if not is_jtag_port(p):
            return p.device
    return None


def find_any_port():
    return find_otg_port() or find_jtag_port()


def wait_for(predicate, timeout: float, what: str):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = predicate()
        if found:
            time.sleep(0.5)  # macOS: the node appears a beat before it is usable
            return found
        time.sleep(0.2)
    raise SystemExit(f"[flash] timed out after {timeout:.0f}s waiting for {what}")


# --- download mode ----------------------------------------------------------
def enter_download_mode() -> tuple[str, str]:
    """Return (port, esptool --before mode) for a board ready to be flashed."""
    otg = find_otg_port()
    if otg:
        log(f"OTG-mode app on {otg}: asking it to reboot into download mode")
        deadline = time.monotonic() + DOWNLOAD_PORT_TIMEOUT
        last_err = None
        while time.monotonic() < deadline:
            jtag = find_jtag_port()
            if jtag:
                time.sleep(0.5)
                log(f"download mode up on {jtag}")
                return jtag, "no-reset"
            port = find_otg_port()
            if port:
                try:
                    with serial.Serial(port, CONSOLE_BAUD, timeout=1) as s:
                        s.write(b"\nbootloader\n")
                        s.flush()
                except (serial.SerialException, OSError) as e:
                    last_err = e
            time.sleep(0.5)
        hint = f" (last error: {last_err})" if last_err else ""
        raise SystemExit(
            f"[flash] the app never entered download mode{hint}.\n"
            "        Close any serial monitor, or do it by hand: hold BOOT, tap RESET,\n"
            "        release BOOT, then re-run.")

    jtag = find_jtag_port()
    if jtag:
        # ROM loader or the launcher: esptool's DTR/RTS sequence handles both.
        log(f"USB-Serial-JTAG on {jtag}: esptool will auto-reset into download mode")
        return jtag, "default-reset"

    raise SystemExit("[flash] no Espressif USB device found. Is the board plugged in "
                     "(data cable, not charge-only)?")


# --- esptool ----------------------------------------------------------------
def esptool(args: list[str]) -> None:
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3"] + args
    log("esptool " + " ".join(args))
    res = subprocess.run(cmd)
    if res.returncode != 0:
        raise SystemExit(f"[flash] esptool failed (exit {res.returncode})")


def write_images(port: str, before: str, images: list[tuple[int, str | bytes]],
                 baud: int = 921600) -> None:
    """One esptool session writing every (offset, file-or-bytes) pair."""
    tmpfiles = []
    try:
        pairs: list[str] = []
        for offset, src in images:
            if isinstance(src, bytes):
                fd, path = tempfile.mkstemp(suffix=".bin")
                with os.fdopen(fd, "wb") as f:
                    f.write(src)
                tmpfiles.append(path)
                src = path
            if not os.path.isfile(src):
                raise SystemExit(f"[flash] no such image: {src}")
            log(f"  {hex(offset):>10}  {os.path.getsize(src):>8} B  {src}")
            pairs += [hex(offset), src]
        esptool([
            "--port", port, "--baud", str(baud),
            "--before", before, "--after", "no-reset",
            "write-flash", "-z",
            "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep",
        ] + pairs)
    finally:
        for path in tmpfiles:
            try:
                os.unlink(path)
            except OSError:
                pass


def clear_force_download_boot(port: str) -> None:
    esptool(["--port", port, "--before", "no-reset", "--after", "no-reset",
             "write-mem", hex(RTC_CNTL_OPTION1_REG), "0x0",
             hex(RTC_CNTL_FORCE_DOWNLOAD_BOOT)])


def reset_into_app(port: str) -> None:
    """Pulse EN with GPIO0 held high (DTR deasserted before open)."""
    log("resetting into the application")
    s = serial.Serial()
    s.port = port
    s.baudrate = CONSOLE_BAUD
    s.dtr = False
    s.rts = False
    s.open()
    try:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    finally:
        s.close()


def finish(port: str) -> None:
    clear_force_download_boot(port)
    reset_into_app(port)


# --- console ----------------------------------------------------------------
def console_port(timeout: float = CONSOLE_PORT_TIMEOUT) -> str:
    return wait_for(find_any_port, timeout, "a console port")


def console(text: str | None, wait: float, stop: str | None = None,
            port: str | None = None, quiet: bool = False) -> str:
    """Send one line (or nothing) and echo what comes back for `wait` seconds.

    Stops early once a line contains `stop`. Returns everything received.
    """
    port = port or console_port()
    out: list[str] = []
    try:
        with serial.Serial(port, CONSOLE_BAUD, timeout=0.2) as s:
            if text is not None:
                time.sleep(0.3)
                s.write(("\n" + text + "\n").encode())
                s.flush()
            deadline = time.monotonic() + wait
            buf = b""
            while time.monotonic() < deadline:
                chunk = s.read(512)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    decoded = line.decode(errors="replace").rstrip("\r")
                    out.append(decoded)
                    if not quiet:
                        print(decoded, flush=True)
                    if stop and stop in decoded:
                        return "\n".join(out)
    except (serial.SerialException, OSError):
        # The device reset (or re-enumerated) under us; what we have is what there is.
        log("device went away (reset?)")
    return "\n".join(out)


def follow(seconds: float, after_port: str | None = None) -> None:
    """Wait for the device to come (back) up and stream its console for `seconds`.

    Survives the device resetting mid-way (it re-waits for a port), so a boot that
    goes launcher -> app -> crash -> launcher is seen end to end. Note that
    OpenTrailPaper brings its USB up only after mounting the SD card, so its first
    seconds of boot log exist only in its SD log (/logs/...); everything from the
    USB enumeration on is streamed here.
    """
    if after_port:
        t0 = time.monotonic()
        while time.monotonic() - t0 < 6 and os.path.exists(after_port):
            time.sleep(0.1)
    end = time.monotonic() + seconds
    port = wait_for(find_any_port, CONSOLE_PORT_TIMEOUT, "the device to come back on USB")
    log(f"following {port} for {seconds:.0f}s (Ctrl-C to stop)")
    try:
        while time.monotonic() < end:
            console(None, end - time.monotonic(), port=port)
            if time.monotonic() >= end:
                break
            try:
                port = wait_for(find_any_port, max(1.0, min(CONSOLE_PORT_TIMEOUT, end - time.monotonic())),
                                "the device to come back on USB")
                log(f"following {port}")
            except SystemExit:
                break
    except KeyboardInterrupt:
        pass


def wait_for_launcher_console(timeout: float = CONSOLE_PORT_TIMEOUT) -> str:
    """After a reset the launcher comes up on the JTAG port; give USB time to enumerate."""
    port = wait_for(find_jtag_port, timeout, "the launcher's USB console")
    time.sleep(1.0)
    return port


# --- commands ---------------------------------------------------------------
def cmd_ports(_args) -> int:
    ports = esp_ports()
    if not ports:
        print("no Espressif USB devices")
        return 1
    for p in ports:
        kind = ("USB-Serial-JTAG (ROM loader or launcher)" if is_jtag_port(p)
                else "OTG-mode app (OpenTrailPaper)")
        print(f"{p.device}  {p.product or '?'}  -> {kind}")
    return 0


def cmd_partitions(_args) -> int:
    print(f"{'name':<10} {'type':<5} {'subtype':<9} {'offset':>10} {'size':>10}  end")
    for p in parse_partitions():
        end = p["offset"] + p["size"]
        print(f"{p['name']:<10} {p['type']:<5} {p['subtype']:<9} "
              f"{hex(p['offset']):>10} {hex(p['size']):>10}  {hex(end)}  "
              f"({p['size'] // 1024} KB)")
    return 0


def blank(size: int) -> bytes:
    return b"\xff" * size


def cmd_system(args) -> int:
    build = args.build_dir
    images: list[tuple[int, str | bytes]] = [
        (BOOTLOADER_OFFSET, os.path.join(build, "bootloader.bin")),
        (PARTITION_TABLE_OFFSET, os.path.join(build, "partitions.bin")),
    ]
    od = otadata_partition()
    images.append((od["offset"], blank(od["size"])))
    if args.erase_nvs:
        for p in parse_partitions():
            if p["subtype"] == "nvs":
                images.append((p["offset"], blank(p["size"])))
    factory = app_slot("factory")
    images.append((factory["offset"], os.path.join(build, "firmware.bin")))
    port, before = enter_download_mode()
    log("writing bootloader, partition table, blank otadata and the launcher")
    write_images(port, before, images, args.baud)
    finish(port)
    if not args.no_wait:
        console(None, 6.0, port=wait_for_launcher_console())
    return 0


def cmd_launcher(args) -> int:
    factory = app_slot("factory")
    od = otadata_partition()
    fw = args.firmware or os.path.join(args.build_dir, "firmware.bin")
    check_fits(fw, factory)
    port, before = enter_download_mode()
    write_images(port, before, [(od["offset"], blank(od["size"])), (factory["offset"], fw)],
                 args.baud)
    finish(port)
    if not args.no_wait:
        console(None, 6.0, port=wait_for_launcher_console())
    return 0


def check_fits(fw: str, slot: dict) -> None:
    if not os.path.isfile(fw):
        raise SystemExit(f"[flash] no such firmware image: {fw}")
    size = os.path.getsize(fw)
    if size > slot["size"]:
        raise SystemExit(f"[flash] {fw} is {size} bytes but slot {slot['name']} holds "
                         f"{slot['size']} bytes")


def cmd_app(args) -> int:
    slot = app_slot(args.slot)
    if slot["name"] == "factory":
        raise SystemExit("[flash] use `launcher` to write the factory slot")
    check_fits(args.firmware, slot)
    od = otadata_partition()
    port, before = enter_download_mode()
    log(f"installing {args.firmware} into {slot['name']} at {hex(slot['offset'])}")
    write_images(port, before, [(od["offset"], blank(od["size"])), (slot["offset"], args.firmware)],
                 args.baud)
    finish(port)

    # The device now boots the launcher, which holds its autostart for a moment
    # when USB power is present precisely so this can get a word in.
    lport = wait_for_launcher_console()
    if args.name or args.version:
        name = args.name or slot["name"]
        version = args.version or ""
        console(f'register {slot["name"]} "{name}" "{version}"', 2.0, stop="[launcher] registered",
                port=lport)
    if args.boot:
        console(f"boot {slot['name']}", 3.0, stop="[launcher] booting", port=lport)
        if args.follow > 0:
            follow(args.follow, after_port=lport)
    else:
        console("menu", 2.0, stop="[launcher] menu", port=lport)
        log(f"installed. Start it with: tools/flash.py boot {slot['name']}")
    return 0


def cmd_cmd(args) -> int:
    console(args.text, args.wait)
    return 0


def cmd_boot(args) -> int:
    slot = app_slot(args.slot)
    port = console_port()
    console(f"boot {slot['name']}", 3.0, stop="[launcher] booting", port=port)
    if args.follow > 0:
        follow(args.follow, after_port=port)
    return 0


def cmd_follow(args) -> int:
    follow(args.wait)
    return 0


def cmd_syscheck(args) -> int:
    console("syscheck", args.wait, stop="[syscheck] done")
    return 0


def cmd_monitor(_args) -> int:
    port = console_port()
    log(f"monitoring {port} (Ctrl-C to stop)")
    try:
        console(None, float("inf"), port=port)
    except KeyboardInterrupt:
        pass
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="list connected Espressif devices").set_defaults(fn=cmd_ports)
    sub.add_parser("partitions", help="print the shared flash layout").set_defaults(fn=cmd_partitions)

    s = sub.add_parser("system", help="full install: bootloader, partition table, launcher")
    s.add_argument("--build-dir", default=LAUNCHER_BUILD)
    s.add_argument("--erase-nvs", action="store_true", help="also wipe all settings")
    s.add_argument("--no-wait", action="store_true", help="do not tail the boot log")
    s.set_defaults(fn=cmd_system)

    s = sub.add_parser("launcher", help="re-flash only the launcher (factory slot)")
    s.add_argument("--build-dir", default=LAUNCHER_BUILD)
    s.add_argument("--firmware", default=None)
    s.add_argument("--no-wait", action="store_true")
    s.set_defaults(fn=cmd_launcher)

    s = sub.add_parser("app", help="install an app image into an OTA slot")
    s.add_argument("slot", help="ota_0 | ota_1 (or 0 | 1)")
    s.add_argument("firmware", help="path to the app's firmware.bin")
    s.add_argument("--name", help="display name for the launcher menu")
    s.add_argument("--version", help="version string for the launcher menu")
    s.add_argument("--boot", action="store_true", help="start the app right after installing")
    s.add_argument("--follow", type=float, default=30.0, metavar="SEC",
                   help="with --boot: stream the app's console for SEC seconds (0 = off, default 30)")
    s.set_defaults(fn=cmd_app)

    s = sub.add_parser("cmd", help="send a console command and print the reply")
    s.add_argument("text")
    s.add_argument("--wait", type=float, default=3.0, help="seconds to listen (default 3)")
    s.set_defaults(fn=cmd_cmd)

    s = sub.add_parser("boot", help="ask the launcher to start a slot and follow its boot log")
    s.add_argument("slot")
    s.add_argument("--follow", type=float, default=30.0, metavar="SEC",
                   help="stream the app's console for SEC seconds after the switch (0 = off, default 30)")
    s.set_defaults(fn=cmd_boot)

    s = sub.add_parser("follow", help="wait for the device to (re)appear on USB and stream its console")
    s.add_argument("--wait", type=float, default=30.0, help="seconds to stream (default 30)")
    s.set_defaults(fn=cmd_follow)

    s = sub.add_parser("syscheck", help="run the launcher's system check and print the report")
    s.add_argument("--wait", type=float, default=120.0)
    s.set_defaults(fn=cmd_syscheck)

    sub.add_parser("monitor", help="serial monitor").set_defaults(fn=cmd_monitor)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
