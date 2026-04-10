#pragma once

#include <Arduino.h>

namespace AppConfig {
// Replace these with your real station values.
static constexpr const char* kCallsign = "N0CALL-7";
static constexpr const char* kDestination = "APLRT1";
static constexpr const char* kPath = "WIDE1-1";
static constexpr const char* kComment = "T-Deck APRS";

static constexpr char kSymbolTable = '/';
static constexpr char kSymbol = '>';

static constexpr uint32_t kBeaconIntervalMs = 120000;
static constexpr uint32_t kNoFixLogIntervalMs = 30000;

static constexpr uint32_t kSerialBaud = 115200;
static constexpr uint32_t kGpsBaud = 9600;

static constexpr float kFrequencyMhz = 433.775f;
static constexpr int kSpreadingFactor = 12;
static constexpr float kBandwidthKhz = 125.0f;
static constexpr int kCodingRate = 5;
static constexpr int kTxPowerDbm = 20;

static constexpr uint8_t kLoRaAprsPrefix0 = 0x3C;
static constexpr uint8_t kLoRaAprsPrefix1 = 0xFF;
static constexpr uint8_t kLoRaAprsPrefix2 = 0x01;
}  // namespace AppConfig
