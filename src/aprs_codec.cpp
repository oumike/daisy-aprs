#include "aprs_codec.h"

#include <math.h>

namespace {
String formatLatitude(double latitude) {
  const char hemisphere = latitude >= 0.0 ? 'N' : 'S';
  const double absVal = fabs(latitude);
  const int degrees = static_cast<int>(absVal);
  const double minutes = (absVal - static_cast<double>(degrees)) * 60.0;

  char out[16];
  snprintf(out, sizeof(out), "%02d%05.2f%c", degrees, minutes, hemisphere);
  return String(out);
}

String formatLongitude(double longitude) {
  const char hemisphere = longitude >= 0.0 ? 'E' : 'W';
  const double absVal = fabs(longitude);
  const int degrees = static_cast<int>(absVal);
  const double minutes = (absVal - static_cast<double>(degrees)) * 60.0;

  char out[16];
  snprintf(out, sizeof(out), "%03d%05.2f%c", degrees, minutes, hemisphere);
  return String(out);
}

String formatCourseSpeed(double courseDeg, double speedKnots) {
  int course = static_cast<int>(round(courseDeg));
  if (course < 0) {
    course = 0;
  }
  if (course > 360) {
    course = 360;
  }

  int speed = static_cast<int>(round(speedKnots));
  if (speed < 0) {
    speed = 0;
  }

  char out[12];
  snprintf(out, sizeof(out), "%03d/%03d", course, speed);
  return String(out);
}

String formatAltitude(long altitudeFeet) {
  long clamped = altitudeFeet;
  if (clamped < 0) {
    clamped = 0;
  }
  if (clamped > 999999) {
    clamped = 999999;
  }

  char out[12];
  snprintf(out, sizeof(out), "/A=%06ld", clamped);
  return String(out);
}
}  // namespace

namespace AprsCodec {

String normalizeCallsign(const String& input) {
  String normalized = input;
  normalized.trim();
  normalized.toUpperCase();
  return normalized;
}

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
                           const String& comment) {
  String packet;
  packet.reserve(180);
  packet += normalizeCallsign(source);
  packet += '>';
  packet += normalizeCallsign(destination);

  if (path.length() > 0) {
    packet += ',';
    packet += path;
  }

  packet += ':';
  packet += '!';
  packet += formatLatitude(latitude);
  packet += symbolTable;
  packet += formatLongitude(longitude);
  packet += symbol;
  packet += formatCourseSpeed(courseDeg, speedKnots);
  packet += formatAltitude(altitudeFeet);

  if (comment.length() > 0) {
    packet += ' ';
    packet += comment;
  }

  return packet;
}

}  // namespace AprsCodec
