#pragma once

#include <Arduino.h>

namespace AprsCodec {
String normalizeCallsign(const String& input);
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
