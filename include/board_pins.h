#pragma once

// LilyGO T-Deck (ESP32-S3 + SX1262)
namespace BoardPins {
constexpr int RADIO_SCLK = 40;
constexpr int RADIO_MISO = 38;
constexpr int RADIO_MOSI = 41;
constexpr int RADIO_CS = 9;
constexpr int RADIO_RST = 17;
constexpr int RADIO_DIO1 = 45;
constexpr int RADIO_BUSY = 13;

constexpr int GPS_RX = 43;
constexpr int GPS_TX = 44;

constexpr int BOARD_POWERON = 10;
constexpr int BOARD_SDCARD_CS = 39;
constexpr int TFT_CS_PIN = 12;
constexpr int TFT_BACKLIGHT_PIN = 42;
}  // namespace BoardPins
