#pragma once
// Pin map of the LilyGO T5S3 4.7" e-paper PRO, the subset Paperback touches.
// Mirrors launcher/src/board.h; the e-paper data bus itself is described by
// EPD_Painter's EPD_LILYGO_T5_S3_GPS_PRESET.
#define BOARD_SDA            39
#define BOARD_SCL            40

#define BOARD_TOUCH_INT      3
#define BOARD_TOUCH_RST      9

// SPI shared by the SD card and the SX1262 LoRa radio (kept deselected).
#define BOARD_SPI_MISO       21
#define BOARD_SPI_MOSI       13
#define BOARD_SPI_SCLK       14
#define BOARD_SD_CS          12
#define BOARD_LORA_CS        46

// PT4103 front-light driver, PWM on this pin.
#define BOARD_BL_EN          11

// BOOT button: also the deep-sleep wake pin.
#define BOARD_BOOT_BTN       0

// XL9555 expander: side button (LOW = pressed) and the GPS/LoRa 3V3 rail.
#define IOEXP_PIN_RADIO_POWER 0
#define IOEXP_PIN_SIDE_BUTTON 10

#define ADDR_IOEXP           0x20
#define ADDR_RTC             0x51
#define ADDR_GAUGE           0x55
#define ADDR_CHARGER         0x6B
