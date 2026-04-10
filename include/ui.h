#pragma once

#include <Arduino.h>

namespace UI {
void begin();
void showSplash(const String& title, const String& subtitle);
void setRadioState(bool isReady);
void setGpsState(bool hasFix, int satellites, float speedKmh);
void noteRxPacket(const String& packet, float rssi, float snr);
void noteTxPacket(const String& packet);
void noteStatus(const String& status);
void render(bool force = false);
}  // namespace UI
