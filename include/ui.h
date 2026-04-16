#pragma once

#include <Arduino.h>

namespace UI {
enum class Screen : uint8_t {
	Main = 0,
	Conversations = 1,
};

void begin();
void showSplash(const String& title, const String& subtitle);
void setCallsign(const String& callsign);
void setRadioState(bool isReady);
void setWifiState(bool connected, bool apMode);
void setBatteryPercent(int percent);
void setGpsState(bool hasFix, int satellites, float speedKmh);
void noteRxPacket(const String& packet, float rssi, float snr);
void addConversation(const String& peer, const String& preview);
void setConversationComposer(bool active,
							bool enteringMessage,
							const String& callsign,
							const String& message);
void scrollRxNewer();
void scrollRxOlder();
void scrollRxPageNewer();
void scrollRxPageOlder();
void resetRxScroll();
void showScreen(Screen screen);
void nextScreen();
void previousScreen();
Screen currentScreen();
void noteTxPacket(const String& packet);
void noteStatus(const String& status);
void render(bool force = false);
}  // namespace UI
