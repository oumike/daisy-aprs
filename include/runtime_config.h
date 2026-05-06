#pragma once

#include <Arduino.h>

// Persisted device settings used by radio, UI, and web configuration.
struct RuntimeConfig {
  // APRS identity and canned message content.
  char callsign[16];
  char destination[16];
  char path[32];
  char comment[80];
  char aprsphMessage[32];
  char hotgMessage[32];

  // APRS map symbol details.
  char symbolTable;
  char symbol;

  // Timing for beacon and display behavior.
  uint32_t beaconIntervalMs;
  uint32_t noFixLogIntervalMs;
  uint16_t screenTimeoutSec;

  // LoRa modulation and RF power settings.
  float frequencyMhz;
  int spreadingFactor;
  float bandwidthKhz;
  int codingRate;
  int txPowerDbm;

  // Manual location fallback when GPS is unavailable.
  bool allowManualPosition;
  int32_t manualLatE7;
  int32_t manualLonE7;
  int32_t manualAltMeters;

  // Home Wi-Fi credentials used by the web config service.
  char wifiSsid[64];
  char wifiPass[64];
};

// Initializes cfg with compile-time defaults.
void runtimeConfigSetDefaults(RuntimeConfig& cfg);

// Loads persisted config and applies sanitization/clamping.
void runtimeConfigLoad(RuntimeConfig& cfg);

// Saves a sanitized copy of cfg to NVS preferences.
bool runtimeConfigSave(const RuntimeConfig& cfg);

// Clears persisted config and returns defaults.
bool runtimeConfigFactoryReset(RuntimeConfig& cfg);
