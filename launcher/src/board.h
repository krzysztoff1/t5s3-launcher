#pragma once
// Pin map and I2C addresses of the LilyGO T5S3 4.7" e-paper PRO, taken from
// LilyGO's factory utilities.h and OpenTrailPaper's config.h. The e-paper data
// bus itself is described by EPD_Painter's EPD_LILYGO_T5_S3_GPS_PRESET.

// Shared I2C: touch (GT911), RTC (PCF8563), fuel gauge (BQ27220), charger
// (BQ25896), IO expander (XL9555/PCA9535, also the panel's power switches),
// panel PMIC (TPS65185).
#define BOARD_SDA            39
#define BOARD_SCL            40

#define BOARD_TOUCH_INT      3
#define BOARD_TOUCH_RST      9

// SPI shared by the SD card and the SX1262 LoRa radio.
#define BOARD_SPI_MISO       21
#define BOARD_SPI_MOSI       13
#define BOARD_SPI_SCLK       14
#define BOARD_SD_CS          12
#define BOARD_LORA_CS        46
#define BOARD_LORA_IRQ       10
#define BOARD_LORA_RST       1
#define BOARD_LORA_BUSY      47
#define BOARD_LORA_TCXO_V    2.4f

// GPS UART (u-blox MIA-M10Q at 38400 or L76K/CASIC at 9600, autodetected).
#define BOARD_GPS_RXD        44
#define BOARD_GPS_TXD        43

// PT4103 front-light driver, PWM on this pin.
#define BOARD_BL_EN          11

// BOOT button. Also the ROM's download-mode strap and the deep-sleep wake pin.
#define BOARD_BOOT_BTN       0
#define BOARD_PCA9535_INT    38

// XL9555 expander pins: 3V3 rail for GPS + LoRa, and the side button (LOW =
// pressed). The side button is the one to hold through a RESET to reach the
// menu: GPIO0 cannot serve, since holding it through a reset is download mode.
#define IOEXP_PIN_RADIO_POWER 0
#define IOEXP_PIN_SIDE_BUTTON 10

#define ADDR_IOEXP           0x20
#define ADDR_RTC             0x51
#define ADDR_GAUGE           0x55
#define ADDR_TOUCH_L         0x5D
#define ADDR_TOUCH_H         0x14
#define ADDR_TPS65185        0x68
#define ADDR_CHARGER         0x6B
