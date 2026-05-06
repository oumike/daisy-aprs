#pragma once

#include <Arduino.h>

namespace AprsCodec {
// Trims and uppercases a callsign-like value.
String normalizeCallsign(const String& input);

// Builds a TNC2 APRS position packet.
// Example: CALL>DST,PATH:!4903.50N/07201.75W123/045/A=001234 comment
String buildPositionPacket(const String& source,
                           const String& destination,
                           const String& path,
                           double latitude,
                           double longitude,
                           double courseDeg,
                           double speedKnots,
                           long altitudeFeet,
                           char symbolTable,
                           char symbol,
                           const String& comment);
}  // namespace AprsCodec
