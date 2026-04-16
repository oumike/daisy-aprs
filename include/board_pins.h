#pragma once

// Heltec WiFi LoRa 32 V4 + Expansion Kit (touchscreen + optional L76K GNSS)
namespace BoardPins {
constexpr int RADIO_SCLK = 9;
constexpr int RADIO_MISO = 11;
constexpr int RADIO_MOSI = 10;
constexpr int RADIO_CS = 8;
constexpr int RADIO_RST = 12;
constexpr int RADIO_DIO1 = 14;
constexpr int RADIO_BUSY = 13;

// L76K GNSS on Heltec V4 variant definition.
constexpr int GPS_RX = 39;  // MCU RX <- GPS TX
constexpr int GPS_TX = 38;  // MCU TX -> GPS RX
constexpr int GPS_RESET = 42;
constexpr int GPS_ENABLE = 34;
constexpr int GPS_STANDBY = 40;
constexpr int GPS_PPS = 41;
constexpr int GPS_ENABLE_ACTIVE = LOW;
constexpr int GPS_RESET_ACTIVE = LOW;

// VEXT controls peripheral rail on Heltec V4 (active LOW).
constexpr int BOARD_POWERON = 36;
constexpr int BOARD_POWERON_ACTIVE = LOW;
constexpr int BOARD_SDCARD_CS = -1;
constexpr int TFT_CS_PIN = 15;
constexpr int TFT_BACKLIGHT_PIN = 21;

// CHSC6x touch controller on expansion kit.
constexpr int TOUCH_SDA = 47;
constexpr int TOUCH_SCL = 48;
constexpr int TOUCH_INT = -1;
constexpr int TOUCH_RST = 44;

// Battery sense (voltage divider into ADC).
constexpr int BATTERY_ADC = 4;
constexpr float BATTERY_DIVIDER = 2.0f;
constexpr float BATTERY_VMIN = 3.20f;
constexpr float BATTERY_VMAX = 4.20f;
}  // namespace BoardPins
