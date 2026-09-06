# Hardware notes — LilyGO T5S3 4.7" e-paper PRO

ESP32-S3-WROOM-1, 16 MB flash, 8 MB octal PSRAM. 960×540 e-paper driven in
540×960 portrait. Pins in `launcher/src/board.h`.

| Function | Pins / address |
|---|---|
| I²C (touch, RTC, gauge, charger, expander, panel PMIC) | SDA 39, SCL 40 |
| GT911 touch | INT 3, RST 9, I²C 0x5D or 0x14 (moves with strapping) |
| PCF8563 RTC | 0x51 |
| BQ27220 fuel gauge | 0x55 |
| BQ25896 charger | 0x6B |
| XL9555 / PCA9535 expander | 0x20 — also the panel's power switches |
| TPS65185 panel PMIC | 0x68 |
| SD card (SPI, shared with LoRa) | SCLK 14, MISO 21, MOSI 13, CS 12 |
| SX1262 LoRa | CS 46, IRQ 10, RST 1, BUSY 47, TCXO 2.4 V |
| GPS UART | RX 44, TX 43 (u-blox at 38400 or L76K/CASIC at 9600) |
| Front light (PT4103) | PWM on GPIO 11 |
| BOOT button | GPIO 0, LOW = pressed |
| Side button | expander pin 10, LOW = pressed |
| GPS + LoRa 3V3 rail | expander pin 0, HIGH = on |

## Quirks that shape the launcher

* **GPIO0 is a strapping pin.** Holding it through a RESET enters the ROM
  download mode, so it cannot be the "hold for menu" button at reset. The side
  button (on the expander) is. GPIO0 is still the wake-from-deep-sleep pin and
  the menu's next/select button once the launcher is running.
* **GPIO48 is the panel's CKV line** in the EPD_Painter preset. Do not treat it
  as a free button.
* **EPD_Painter must be given our `Wire`** (`cfg.i2c.wire = &Wire`) and
  `setAutoShutdown(false)`; otherwise it either fails powerctl init or never
  returns from `begin()`. Its 129 KB fast buffer is pushed to PSRAM by making a
  large internal block temporarily unavailable during `begin()`, so Wi-Fi and
  BLE fit afterwards. All three lessons come from OpenTrailPaper's
  `src/epd_compat.cpp`.
* **Touch orientation is unverified** on hardware for this rotation. If the
  system check's touch step reports coordinates mirrored against where you
  tapped, run `tools/flash.py cmd "touchflip on"` and reboot.
* **Two USB personalities.** The launcher runs USB-Serial-JTAG (esptool
  auto-reset works). OpenTrailPaper runs USB-OTG for mass storage; it must be
  asked to reboot into download mode (`bootloader` console command), which
  `tools/flash.py` does automatically.
* **Shared expander.** EPD_Painter's power control and our side-button/radio
  rail code both talk to the XL9555 at 0x20. OpenTrailPaper does the same and it
  works in practice; keep expander writes to pin-level calls.
