#pragma once

#include <Arduino.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace AppConfig {
static constexpr const char* kVersion = APP_VERSION;
// Replace these with your real station values.
static constexpr const char* kCallsign = "N0CALL-7";
static constexpr const char* kDestination = "APLRT1";
static constexpr const char* kPath = "WIDE1-1";
static constexpr const char* kComment = "Heltec V4 APRS";
static constexpr const char* kAprsphMessage = "Net";

static constexpr char kSymbolTable = '/';
static constexpr char kSymbol = '>';

static constexpr uint32_t kBeaconIntervalMs = 3600000;
static constexpr uint32_t kNoFixLogIntervalMs = 30000;

static constexpr uint32_t kSerialBaud = 115200;
static constexpr uint32_t kGpsBaud = 9600;

// Leave empty to start in AP fallback until configured via web UI.
static constexpr const char* kDefaultWifiSsid = "";
static constexpr const char* kDefaultWifiPass = "";

static constexpr float kFrequencyMhz = 433.775f;
static constexpr int kSpreadingFactor = 12;
static constexpr float kBandwidthKhz = 125.0f;
static constexpr int kCodingRate = 5;
static constexpr int kTxPowerDbm = 20;

static constexpr uint8_t kLoRaAprsPrefix0 = 0x3C;
static constexpr uint8_t kLoRaAprsPrefix1 = 0xFF;
static constexpr uint8_t kLoRaAprsPrefix2 = 0x01;
}  // namespace AppConfig
