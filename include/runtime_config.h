#pragma once

#include <Arduino.h>

struct RuntimeConfig {
  char callsign[16];
  char destination[16];
  char path[32];
  char comment[80];
  char aprsphMessage[32];

  char symbolTable;
  char symbol;

  uint32_t beaconIntervalMs;
  uint32_t noFixLogIntervalMs;

  float frequencyMhz;
  int spreadingFactor;
  float bandwidthKhz;
  int codingRate;
  int txPowerDbm;

  bool allowManualPosition;
  int32_t manualLatE7;
  int32_t manualLonE7;
  int32_t manualAltMeters;

  char wifiSsid[64];
  char wifiPass[64];
};

void runtimeConfigSetDefaults(RuntimeConfig& cfg);
void runtimeConfigLoad(RuntimeConfig& cfg);
bool runtimeConfigSave(const RuntimeConfig& cfg);
