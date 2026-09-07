# PNGdec 1.0.3, vendored

Copy of https://github.com/bitbank2/PNGdec (Apache-2.0, see LICENSE) with two
changes:

* `src/s3_simd_rgb565.S` removed: it includes `dsps_fft2r_platform.h` from
  ESP-DSP, which the pinned Arduino core (2.0.14 / IDF 4.4) does not ship, so
  the registry package does not build for this board.
* `png.inl`: the ESP32-S3 SIMD RGB565 path is now behind `PNGDEC_USE_S3_SIMD`
  in addition to `ARDUINO_ESP32S3_DEV`, so nothing references the removed
  routine. We decode covers to 8-bit grey; the RGB565 fast path is irrelevant.
