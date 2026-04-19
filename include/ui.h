#pragma once

#include <Arduino.h>

namespace UI {
enum class Screen : uint8_t {
	Main = 0,
	Conversations = 1,
};

enum class TouchButton : uint8_t {
	None = 0,
	ScreenToggle,
	Beacon,
	Test,
	Aprsph,
	Thurs,
	Wx,
	LogScrollUp,
	LogScrollDown,
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
void scrollLogNewer();
void scrollLogOlder();
void scrollLogPageNewer();
void scrollLogPageOlder();
void resetLogScroll();
bool openMainDetailAt(int16_t x, int16_t y);
bool handleMainDetailTouch(int16_t x, int16_t y);
bool isMainDetailActive();
bool openLogDetailAt(int16_t x, int16_t y);
bool handleLogDetailTouch(int16_t x, int16_t y);
bool isLogDetailActive();
bool handleLogScrollButtonTouch(int16_t x, int16_t y);
void showScreen(Screen screen);
void nextScreen();
void previousScreen();
Screen currentScreen();
void flashButton(TouchButton button);
void noteTxPacket(const String& packet);
void noteStatus(const String& status);
void render(bool force = false);
}  // namespace UI
